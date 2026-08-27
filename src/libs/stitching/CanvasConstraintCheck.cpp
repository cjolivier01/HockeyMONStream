#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/TransactionState.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include <fcntl.h>
#include <openssl/evp.h>
#include <png.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <tiffio.h>
#include <unistd.h>

#include "absl/status/status.h"

namespace hm::stitching {
namespace {

namespace fs = std::filesystem;

constexpr size_t kDefaultJetsonMaxLiveStitchCanvasDimension = 8192;
constexpr size_t kHardMaximumArtifactDimension = 32768;
constexpr uint64_t kHardMaximumArtifactPixels = 128ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaximumPtoArtifactBytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t kTiffMetadataAllowanceBytes = 16ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaximumTiffArtifactBytes = kHardMaximumArtifactPixels * 4 + kTiffMetadataAllowanceBytes;
constexpr uint64_t kMaximumPngArtifactBytes = 512ULL * 1024ULL * 1024ULL;
constexpr const char* kStitchTransactionPrefix = ".hstream-stitch-";
constexpr unsigned long kFuseSuperMagic = 0x65735546UL;
constexpr unsigned long kMsDosSuperMagic = 0x4d44UL;
constexpr unsigned long kExFatSuperMagic = 0x2011bab0UL;
constexpr unsigned long kNfsSuperMagic = 0x6969UL;
constexpr unsigned long kSmbSuperMagic = 0x517bUL;
constexpr unsigned long kCifsSuperMagic = 0xff534d42UL;
constexpr unsigned long kSmb2SuperMagic = 0xfe534d42UL;

const std::array<const char*, 9> kGenerationArtifacts = {
    "hm_project.pto",
    "autooptimiser_out.pto",
    "mapping_0000.tif",
    "mapping_0000_x.tif",
    "mapping_0000_y.tif",
    "mapping_0001.tif",
    "mapping_0001_x.tif",
    "mapping_0001_y.tif",
    "seam_file.png",
};

uint64_t maximum_stitch_artifact_bytes(std::string_view name) {
  const auto ends_with = [name](std::string_view suffix) {
    return name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix;
  };
  if (ends_with(".pto"))
    return kMaximumPtoArtifactBytes;
  if (ends_with(".tif"))
    return kMaximumTiffArtifactBytes;
  if (ends_with(".png"))
    return kMaximumPngArtifactBytes;
  if (name == kStitchCanvasProvenanceArtifact)
    return 4096;
  if (name == kStitchGenerationArtifact)
    return 16ULL * 1024ULL;
  return 0;
}

absl::StatusOr<uint64_t> maximum_open_tiff_artifact_bytes(int descriptor, const fs::path& path) {
  const int tiff_descriptor = ::dup(descriptor);
  if (tiff_descriptor < 0)
    return absl::InternalError("Unable to duplicate TIFF artifact descriptor: " + path.string());
  const std::string tiff_name = path.filename().string();
  TIFF* tiff = TIFFFdOpen(tiff_descriptor, tiff_name.c_str(), "r");
  if (tiff == nullptr) {
    ::close(tiff_descriptor);
    return absl::FailedPreconditionError("Unable to parse TIFF artifact header: " + path.string());
  }
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t samples = 0;
  uint16_t bits = 0;
  const bool dimensions_valid =
      TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) && TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFClose(tiff);
  if (!dimensions_valid || width == 0 || height == 0 || samples == 0 || bits == 0 ||
      width > kHardMaximumArtifactDimension || height > kHardMaximumArtifactDimension ||
      static_cast<uint64_t>(width) > kHardMaximumArtifactPixels / height) {
    return absl::ResourceExhaustedError("TIFF artifact dimensions exceed safety limits: " + path.string());
  }
  const uint64_t pixels = static_cast<uint64_t>(width) * height;
  if (pixels > std::numeric_limits<uint64_t>::max() / samples ||
      pixels * samples > (std::numeric_limits<uint64_t>::max() - 7) / bits) {
    return absl::ResourceExhaustedError("TIFF artifact payload size overflows: " + path.string());
  }
  const uint64_t payload_bits = pixels * samples * bits;
  const uint64_t payload_bytes = (payload_bits + 7) / 8;
  return std::min(kMaximumTiffArtifactBytes, payload_bytes + kTiffMetadataAllowanceBytes);
}

absl::Status validate_stitch_artifact_bounds(const fs::path& game_dir, const char* name, bool required) {
  const fs::path path = game_dir / name;
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0) {
    if (errno == ENOENT)
      return required ? absl::NotFoundError("Missing stitch artifact: " + path.string()) : absl::OkStatus();
    return absl::FailedPreconditionError("Unable to open bounded stitch artifact: " + path.string());
  }
  struct DescriptorCleanup {
    int descriptor;
    ~DescriptorCleanup() {
      ::close(descriptor);
    }
  } cleanup{descriptor};
  struct stat metadata{};
  uint64_t maximum_bytes = maximum_stitch_artifact_bytes(name);
  if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size <= 0 || maximum_bytes == 0)
    return absl::FailedPreconditionError("Invalid stitch artifact: " + path.string());
  if (static_cast<uint64_t>(metadata.st_size) > maximum_bytes)
    return absl::ResourceExhaustedError("Oversized stitch artifact: " + path.string());
  if (path.extension() == ".tif") {
    auto tiff_maximum = maximum_open_tiff_artifact_bytes(descriptor, path);
    if (!tiff_maximum.ok())
      return tiff_maximum.status();
    maximum_bytes = *tiff_maximum;
    struct stat verified{};
    if (::fstat(descriptor, &verified) != 0 || metadata.st_dev != verified.st_dev ||
        metadata.st_ino != verified.st_ino || metadata.st_mode != verified.st_mode ||
        metadata.st_size != verified.st_size || metadata.st_mtim.tv_sec != verified.st_mtim.tv_sec ||
        metadata.st_mtim.tv_nsec != verified.st_mtim.tv_nsec || metadata.st_ctim.tv_sec != verified.st_ctim.tv_sec ||
        metadata.st_ctim.tv_nsec != verified.st_ctim.tv_nsec) {
      return absl::AbortedError("TIFF artifact changed while its bounds were inspected: " + path.string());
    }
  }
  if (static_cast<uint64_t>(metadata.st_size) > maximum_bytes)
    return absl::ResourceExhaustedError("Oversized stitch artifact: " + path.string());
  return absl::OkStatus();
}

const std::vector<std::string> kRequiredArtifacts = {
    "hm_project.pto",
    "autooptimiser_out.pto",
    "mapping_0000.tif",
    "mapping_0000_x.tif",
    "mapping_0000_y.tif",
    "mapping_0001.tif",
    "mapping_0001_x.tif",
    "mapping_0001_y.tif",
    "left.png",
    "right.png",
    kStitchCanvasProvenanceArtifact,
};

const std::set<std::string> kPreviousArtifacts = {
    "hm_project.pto",
    "autooptimiser_out.pto",
    "mapping_0000.tif",
    "mapping_0000_x.tif",
    "mapping_0000_y.tif",
    "mapping_0001.tif",
    "mapping_0001_x.tif",
    "mapping_0001_y.tif",
    "left.png",
    "right.png",
    "seam_file.png",
    "panorama.tif",
};

const std::set<std::string> kLegacyArtifacts = {
    "hm_project.pto",
    "autooptimiser_out.pto",
    "mapping_0000.tif",
    "mapping_0000_x.tif",
    "mapping_0000_y.tif",
    "mapping_0001.tif",
    "mapping_0001_x.tif",
    "mapping_0001_y.tif",
    "seam_file.png",
    "panorama.tif",
};

const std::vector<std::string> kArtifacts = [] {
  std::vector<std::string> names = kRequiredArtifacts;
  names.emplace_back("seam_file.png");
  names.emplace_back("panorama.tif");
  names.emplace_back(kStitchGenerationArtifact);
  return names;
}();

const std::set<std::string> kArtifactsWithoutGeneration = [] {
  std::set<std::string> names(kArtifacts.begin(), kArtifacts.end());
  names.erase(kStitchGenerationArtifact);
  return names;
}();

struct TiffPlacement {
  float x_px{0.0f};
  float y_px{0.0f};
  size_t width{0};
  size_t height{0};
};

struct CanvasSize {
  size_t width{0};
  size_t height{0};
};

struct PngLayout {
  int width{0};
  int height{0};
  int offset_x{0};
  int offset_y{0};
  bool has_offset{false};
};

struct CanvasProvenance {
  size_t max_output_width{0};
  size_t max_canvas_dimension{0};
  size_t source_canvas_width{0};
  size_t source_canvas_height{0};
  size_t canvas_width{0};
  size_t canvas_height{0};
  bool max_output_width_applied{false};
  bool max_canvas_dimension_applied{false};
};

std::optional<size_t> positive_environment_size(const char* name) {
  const char* value = std::getenv(name);
  if (!value || !*value)
    return std::nullopt;
  size_t parsed = 0;
  const char* end = value + std::strlen(value);
  const auto result = std::from_chars(value, end, parsed);
  return result.ec == std::errc() && result.ptr == end && parsed > 0 ? std::optional<size_t>(parsed) : std::nullopt;
}

std::optional<size_t> live_canvas_limit_impl() {
  if (const char* allow = std::getenv("HM_ALLOW_OVERSIZED_LIVE_STITCH"); allow && std::strcmp(allow, "1") == 0)
    return std::nullopt;
  if (auto configured = positive_environment_size("HM_MAX_LIVE_STITCH_EGL_DIMENSION"))
    return configured;
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  return kDefaultJetsonMaxLiveStitchCanvasDimension;
#else
  return std::nullopt;
#endif
}

absl::StatusOr<int> lock_canvas_constraint_artifacts_impl(const fs::path& game_dir, bool wait) {
  std::error_code error;
  if (!fs::is_directory(game_dir, error) || error)
    return absl::NotFoundError("Canvas compatibility check requires an existing game directory");
  const fs::path lock_path = game_dir / ".hstream-stitch.lock";
  const int descriptor = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (descriptor < 0)
    return absl::InternalError("Unable to open stitching artifact lock: " + std::string(std::strerror(errno)));
  if (::flock(descriptor, LOCK_EX | (wait ? 0 : LOCK_NB)) != 0) {
    const int lock_error = errno;
    ::close(descriptor);
    if (!wait && (lock_error == EWOULDBLOCK || lock_error == EAGAIN))
      return -1;
    return absl::InternalError("Unable to lock stitching artifacts: " + std::string(std::strerror(lock_error)));
  }
  auto recovery = recover_stitch_transactions_locked(game_dir);
  if (!recovery.ok()) {
    ::flock(descriptor, LOCK_UN);
    ::close(descriptor);
    return recovery;
  }
  return descriptor;
}

bool any_mapping_artifact_exists(const fs::path& game_dir) {
  for (const char* name : {
           "mapping_0000.tif",
           "mapping_0000_x.tif",
           "mapping_0000_y.tif",
           "mapping_0001.tif",
           "mapping_0001_x.tif",
           "mapping_0001_y.tif",
       }) {
    std::error_code error;
    if (fs::exists(game_dir / name, error) && !error)
      return true;
  }
  return false;
}

absl::StatusOr<std::pair<fs::file_time_type, fs::file_time_type>> artifact_group_times(
    const fs::path& game_dir,
    const std::vector<const char*>& names) {
  std::optional<fs::file_time_type> oldest;
  std::optional<fs::file_time_type> newest;
  for (const char* name : names) {
    const fs::path path = game_dir / name;
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error)
      return absl::NotFoundError("Missing stitching artifact: " + path.string());
    const auto time = fs::last_write_time(path, error);
    if (error)
      return absl::InternalError("Unable to inspect stitching artifact timestamp: " + error.message());
    oldest = oldest.has_value() ? std::min(*oldest, time) : time;
    newest = newest.has_value() ? std::max(*newest, time) : time;
  }
  return std::pair<fs::file_time_type, fs::file_time_type>{*oldest, *newest};
}

absl::Status dependency_tree_status(const fs::path& game_dir) {
  const std::array<std::vector<const char*>, 4> levels = {
      std::vector<const char*>{"left.png", "right.png"},
      std::vector<const char*>{"hm_project.pto"},
      std::vector<const char*>{"autooptimiser_out.pto"},
      std::vector<const char*>{
          "mapping_0000.tif",
          "mapping_0000_x.tif",
          "mapping_0000_y.tif",
          "mapping_0001.tif",
          "mapping_0001_x.tif",
          "mapping_0001_y.tif",
      },
  };
  std::optional<fs::file_time_type> parent_newest;
  for (const auto& level : levels) {
    auto times = artifact_group_times(game_dir, level);
    if (!times.ok())
      return times.status();
    if (parent_newest.has_value() && times->first < *parent_newest)
      return absl::FailedPreconditionError("Stitching artifact dependency timestamps are stale");
    parent_newest = times->second;
  }
  return absl::OkStatus();
}

absl::StatusOr<TiffPlacement> read_tiff_placement(const fs::path& path) {
  TIFF* tiff = TIFFOpen(path.c_str(), "r");
  if (!tiff)
    return absl::NotFoundError("Could not open mapping TIFF: " + path.string());
  uint32_t width = 0;
  uint32_t height = 0;
  float x_resolution = 0.0f;
  float y_resolution = 0.0f;
  float x_position = 0.0f;
  float y_position = 0.0f;
  const bool have_dimensions =
      TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) && TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height);
  const bool have_resolution =
      TIFFGetField(tiff, TIFFTAG_XRESOLUTION, &x_resolution) && TIFFGetField(tiff, TIFFTAG_YRESOLUTION, &y_resolution);
  const bool have_position =
      TIFFGetField(tiff, TIFFTAG_XPOSITION, &x_position) && TIFFGetField(tiff, TIFFTAG_YPOSITION, &y_position);
  TIFFClose(tiff);
  const float x_px = x_position * x_resolution;
  const float y_px = y_position * y_resolution;
  if (!have_dimensions || width == 0 || height == 0 || !have_resolution || !have_position ||
      !std::isfinite(x_resolution) || !std::isfinite(y_resolution) || x_resolution <= 0.0f || y_resolution <= 0.0f ||
      !std::isfinite(x_position) || !std::isfinite(y_position) || !std::isfinite(x_px) || !std::isfinite(y_px) ||
      width > kHardMaximumArtifactDimension || height > kHardMaximumArtifactDimension ||
      static_cast<uint64_t>(width) * height > kHardMaximumArtifactPixels) {
    return absl::FailedPreconditionError("Invalid mapping TIFF placement metadata: " + path.string());
  }
  return TiffPlacement{.x_px = x_px, .y_px = y_px, .width = width, .height = height};
}

absl::StatusOr<CanvasSize> mapping_canvas_size(const fs::path& game_dir) {
  auto first = read_tiff_placement(game_dir / "mapping_0000.tif");
  if (!first.ok())
    return first.status();
  auto second = read_tiff_placement(game_dir / "mapping_0001.tif");
  if (!second.ok())
    return second.status();
  const float min_x = std::min(first->x_px, second->x_px);
  const float min_y = std::min(first->y_px, second->y_px);
  const float width = std::max(first->x_px - min_x + first->width, second->x_px - min_x + second->width);
  const float height = std::max(first->y_px - min_y + first->height, second->y_px - min_y + second->height);
  if (!std::isfinite(width) || !std::isfinite(height) || width < 1.0f || height < 1.0f ||
      width > kHardMaximumArtifactDimension || height > kHardMaximumArtifactDimension ||
      width * height > kHardMaximumArtifactPixels) {
    return absl::FailedPreconditionError("Mapping TIFFs produce an invalid canvas");
  }
  return CanvasSize{.width = static_cast<size_t>(width), .height = static_cast<size_t>(height)};
}

absl::StatusOr<CanvasSize> read_remap_tiff_header(const fs::path& path) {
  TIFF* tiff = TIFFOpen(path.c_str(), "r");
  if (!tiff)
    return absl::NotFoundError("Could not open remap TIFF: " + path.string());
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t samples = 0;
  uint16_t bits = 0;
  uint16_t sample_format = SAMPLEFORMAT_UINT;
  uint16_t planar = PLANARCONFIG_CONTIG;
  uint16_t orientation = ORIENTATION_TOPLEFT;
  const bool have_dimensions =
      TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) && TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLEFORMAT, &sample_format);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_PLANARCONFIG, &planar);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_ORIENTATION, &orientation);
  TIFFClose(tiff);
  if (!have_dimensions || width == 0 || height == 0 || width > kHardMaximumArtifactDimension ||
      height > kHardMaximumArtifactDimension || static_cast<uint64_t>(width) * height > kHardMaximumArtifactPixels ||
      samples != 1 || bits != 16 || sample_format != SAMPLEFORMAT_UINT || planar != PLANARCONFIG_CONTIG ||
      orientation != ORIENTATION_TOPLEFT) {
    return absl::FailedPreconditionError("Invalid remap TIFF metadata: " + path.string());
  }
  return CanvasSize{.width = width, .height = height};
}

uint32_t png_crc32(const unsigned char* type, const unsigned char* data, size_t size) {
  uint32_t crc = 0xffffffffU;
  auto update = [&](const unsigned char* bytes, size_t count) {
    for (size_t index = 0; index < count; ++index) {
      crc ^= bytes[index];
      for (int bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  };
  update(type, 4);
  update(data, size);
  return crc ^ 0xffffffffU;
}

absl::StatusOr<PngLayout> read_png_layout(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  const std::array<unsigned char, 8> signature = {137, 80, 78, 71, 13, 10, 26, 10};
  std::array<unsigned char, 8> file_signature{};
  input.read(reinterpret_cast<char*>(file_signature.data()), static_cast<std::streamsize>(file_signature.size()));
  if (!input || file_signature != signature)
    return absl::FailedPreconditionError("Invalid PNG header: " + path.string());
  auto big_endian_u32 = [](const unsigned char* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
        (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
  };

  PngLayout layout;
  bool have_header = false;
  bool have_offset = false;
  bool have_image_data = false;
  bool have_end = false;
  bool first_chunk = true;
  while (input) {
    std::array<unsigned char, 8> chunk_header{};
    input.read(reinterpret_cast<char*>(chunk_header.data()), static_cast<std::streamsize>(chunk_header.size()));
    if (!input)
      break;
    const uint32_t length = big_endian_u32(chunk_header.data());
    const std::string type(reinterpret_cast<const char*>(chunk_header.data() + 4), 4);
    if (first_chunk && type != "IHDR")
      return absl::FailedPreconditionError("PNG IHDR is not the first chunk: " + path.string());
    first_chunk = false;
    std::optional<uint32_t> expected_crc;
    if (type == "IHDR") {
      if (have_header || length != 13)
        return absl::FailedPreconditionError("Invalid PNG IHDR chunk: " + path.string());
      std::array<unsigned char, 13> data{};
      input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
      if (!input)
        return absl::FailedPreconditionError("Truncated PNG IHDR chunk: " + path.string());
      const uint32_t width = big_endian_u32(data.data());
      const uint32_t height = big_endian_u32(data.data() + 4);
      if (width == 0 || height == 0 || width > kHardMaximumArtifactDimension ||
          height > kHardMaximumArtifactDimension ||
          static_cast<uint64_t>(width) * height > kHardMaximumArtifactPixels) {
        return absl::FailedPreconditionError("Invalid PNG dimensions: " + path.string());
      }
      layout.width = static_cast<int>(width);
      layout.height = static_cast<int>(height);
      have_header = true;
      expected_crc = png_crc32(chunk_header.data() + 4, data.data(), data.size());
    } else if (type == "oFFs") {
      if (!have_header || have_offset || have_image_data || length != 9)
        return absl::FailedPreconditionError("Invalid PNG oFFs chunk: " + path.string());
      std::array<unsigned char, 9> data{};
      input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
      if (!input || data[8] != 0)
        return absl::FailedPreconditionError("PNG seam offset is not expressed in pixels: " + path.string());
      auto signed_coordinate = [&](const unsigned char* bytes) {
        const uint32_t value = big_endian_u32(bytes);
        return value <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
            ? static_cast<int64_t>(value)
            : static_cast<int64_t>(value) - (int64_t{1} << 32);
      };
      layout.offset_x = static_cast<int>(signed_coordinate(data.data()));
      layout.offset_y = static_cast<int>(signed_coordinate(data.data() + 4));
      layout.has_offset = true;
      have_offset = true;
      expected_crc = png_crc32(chunk_header.data() + 4, data.data(), data.size());
    } else {
      input.seekg(static_cast<std::streamoff>(length), std::ios::cur);
      if (!input)
        return absl::FailedPreconditionError("Truncated PNG chunk: " + path.string());
    }
    std::array<unsigned char, 4> crc{};
    input.read(reinterpret_cast<char*>(crc.data()), static_cast<std::streamsize>(crc.size()));
    if (!input)
      return absl::FailedPreconditionError("Truncated PNG chunk CRC: " + path.string());
    if (expected_crc.has_value() && big_endian_u32(crc.data()) != *expected_crc)
      return absl::FailedPreconditionError("PNG " + type + " chunk has an invalid CRC: " + path.string());
    if (type == "IDAT")
      have_image_data = true;
    if (type == "IEND") {
      have_end = true;
      break;
    }
  }
  if (!have_header || !have_end)
    return absl::FailedPreconditionError("PNG seam is missing required chunks: " + path.string());
  return layout;
}

absl::Status validate_nonuniform_png(const fs::path& path, const PngLayout& expected_layout) {
  FILE* input = std::fopen(path.c_str(), "rb");
  if (input == nullptr)
    return absl::NotFoundError("Could not open PNG seam: " + path.string());
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png == nullptr ? nullptr : png_create_info_struct(png);
  png_bytep row = static_cast<png_bytep>(std::malloc(static_cast<size_t>(expected_layout.width)));
  if (png == nullptr || info == nullptr || row == nullptr) {
    std::free(row);
    if (png != nullptr)
      png_destroy_read_struct(&png, nullptr, nullptr);
    std::fclose(input);
    return absl::ResourceExhaustedError("Unable to allocate PNG seam decoder state");
  }
  if (setjmp(png_jmpbuf(png))) {
    std::free(row);
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(input);
    return absl::FailedPreconditionError("PNG seam is not decodable: " + path.string());
  }
  png_init_io(png, input);
  png_read_info(png, info);
  const png_uint_32 width = png_get_image_width(png, info);
  const png_uint_32 height = png_get_image_height(png, info);
  const int color_type = png_get_color_type(png, info);
  const int bit_depth = png_get_bit_depth(png, info);
  if (width != static_cast<png_uint_32>(expected_layout.width) ||
      height != static_cast<png_uint_32>(expected_layout.height)) {
    png_error(png, "PNG layout changed during decode");
  }
  if (png_get_interlace_type(png, info) != PNG_INTERLACE_NONE)
    png_error(png, "Interlaced PNG seams are unsupported");
  if (bit_depth == 16)
    png_set_strip_16(png);
  if (color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
    png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png);
  if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_RGB_ALPHA ||
      color_type == PNG_COLOR_TYPE_PALETTE) {
    png_set_rgb_to_gray_fixed(png, PNG_ERROR_ACTION_NONE, -1, -1);
  }
  if ((color_type & PNG_COLOR_MASK_ALPHA) != 0 || png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_strip_alpha(png);
  png_read_update_info(png, info);
  if (png_get_channels(png, info) != 1 || png_get_bit_depth(png, info) != 8 || png_get_rowbytes(png, info) != width) {
    png_error(png, "PNG seam does not decode to 8-bit grayscale");
  }
  unsigned minimum = 255;
  unsigned maximum = 0;
  for (png_uint_32 y = 0; y < height; ++y) {
    png_read_row(png, row, nullptr);
    for (png_uint_32 x = 0; x < width; ++x) {
      minimum = std::min(minimum, static_cast<unsigned>(row[x]));
      maximum = std::max(maximum, static_cast<unsigned>(row[x]));
    }
  }
  png_read_end(png, nullptr);
  std::free(row);
  png_destroy_read_struct(&png, &info, nullptr);
  std::fclose(input);
  if (maximum <= minimum)
    return absl::FailedPreconditionError("PNG seam is uniform: " + path.string());
  return absl::OkStatus();
}

absl::Status validate_canvas_artifact_contract(
    const fs::path& game_dir,
    const CanvasSize& canvas,
    bool validate_seam_payload) {
  auto left = read_tiff_placement(game_dir / "mapping_0000.tif");
  auto right = read_tiff_placement(game_dir / "mapping_0001.tif");
  if (!left.ok())
    return left.status();
  if (!right.ok())
    return right.status();
  CanvasSize left_x;
  CanvasSize left_y;
  CanvasSize right_x;
  CanvasSize right_y;
  auto assign = [&](CanvasSize* output, const char* name) -> absl::Status {
    auto header = read_remap_tiff_header(game_dir / name);
    if (!header.ok())
      return header.status();
    *output = *header;
    return absl::OkStatus();
  };
  auto status = assign(&left_x, "mapping_0000_x.tif");
  if (!status.ok())
    return status;
  status = assign(&left_y, "mapping_0000_y.tif");
  if (!status.ok())
    return status;
  status = assign(&right_x, "mapping_0001_x.tif");
  if (!status.ok())
    return status;
  status = assign(&right_y, "mapping_0001_y.tif");
  if (!status.ok())
    return status;
  if (left_x.width != left_y.width || left_x.height != left_y.height || right_x.width != right_y.width ||
      right_x.height != right_y.height || left_x.width != left->width || left_x.height != left->height ||
      right_x.width != right->width || right_x.height != right->height) {
    return absl::FailedPreconditionError("Stitching placement and remap TIFF dimensions do not match");
  }

  const fs::path seam_path = game_dir / "seam_file.png";
  auto layout = read_png_layout(seam_path);
  if (!layout.ok())
    return layout.status();
  const int64_t right_edge = static_cast<int64_t>(layout->offset_x) + layout->width;
  const int64_t bottom_edge = static_cast<int64_t>(layout->offset_y) + layout->height;
  const bool full_canvas = layout->offset_x == 0 && layout->offset_y == 0 &&
      layout->width == static_cast<int>(canvas.width) && layout->height == static_cast<int>(canvas.height);
  if ((!layout->has_offset && !full_canvas) || layout->offset_x < 0 || layout->offset_y < 0 ||
      right_edge > static_cast<int64_t>(canvas.width) || bottom_edge > static_cast<int64_t>(canvas.height)) {
    return absl::FailedPreconditionError("PNG seam layout does not match the stitched mapping canvas");
  }
  return validate_seam_payload ? validate_nonuniform_png(seam_path, *layout) : absl::OkStatus();
}

absl::StatusOr<size_t> parse_provenance_value(const std::string& line, const char* key) {
  const std::string prefix = std::string(key) + "=";
  if (line.rfind(prefix, 0) != 0 || line.size() == prefix.size())
    return absl::FailedPreconditionError("Invalid canvas provenance field: " + std::string(key));
  size_t value = 0;
  const auto parsed = std::from_chars(line.data() + prefix.size(), line.data() + line.size(), value);
  if (parsed.ec != std::errc() || parsed.ptr != line.data() + line.size())
    return absl::FailedPreconditionError("Invalid canvas provenance value: " + std::string(key));
  return value;
}

absl::StatusOr<std::string> read_bounded_stitch_file(
    const fs::path& path,
    size_t maximum_bytes,
    const char* description) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0) {
    if (errno == ENOENT)
      return absl::NotFoundError(std::string(description) + " is missing");
    return absl::FailedPreconditionError("Unable to open " + std::string(description));
  }
  struct CloseDescriptor {
    int descriptor;
    ~CloseDescriptor() {
      ::close(descriptor);
    }
  } close{descriptor};
  struct stat metadata{};
  if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
      static_cast<uint64_t>(metadata.st_size) > maximum_bytes) {
    return absl::FailedPreconditionError("Invalid " + std::string(description));
  }
  std::string contents(static_cast<size_t>(metadata.st_size), '\0');
  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t count = ::read(descriptor, contents.data() + offset, contents.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return absl::FailedPreconditionError("Unable to read " + std::string(description));
    offset += static_cast<size_t>(count);
  }
  return contents;
}

absl::StatusOr<CanvasProvenance> read_canvas_provenance(const fs::path& game_dir) {
  auto contents = read_bounded_stitch_file(game_dir / "stitching_canvas_provenance", 4 * 1024, "canvas provenance");
  if (!contents.ok())
    return contents.status();
  std::istringstream input(*contents);
  std::vector<std::string> lines;
  for (std::string line; std::getline(input, line);)
    lines.push_back(std::move(line));
  if (!input.eof() || lines.size() != 9 || lines[0] != "version=2")
    return absl::FailedPreconditionError("Invalid canvas provenance format");
  CanvasProvenance provenance;
  auto assign = [&](size_t* destination, size_t line, const char* key) -> absl::Status {
    auto parsed = parse_provenance_value(lines[line], key);
    if (!parsed.ok())
      return parsed.status();
    *destination = *parsed;
    return absl::OkStatus();
  };
  if (auto status = assign(&provenance.max_output_width, 1, "max-output-width"); !status.ok())
    return status;
  if (auto status = assign(&provenance.max_canvas_dimension, 2, "max-canvas-dimension"); !status.ok())
    return status;
  if (auto status = assign(&provenance.source_canvas_width, 3, "source-canvas-width"); !status.ok())
    return status;
  if (auto status = assign(&provenance.source_canvas_height, 4, "source-canvas-height"); !status.ok())
    return status;
  if (auto status = assign(&provenance.canvas_width, 5, "canvas-width"); !status.ok())
    return status;
  if (auto status = assign(&provenance.canvas_height, 6, "canvas-height"); !status.ok())
    return status;
  size_t width_applied = 0;
  size_t dimension_applied = 0;
  if (auto status = assign(&width_applied, 7, "max-output-width-applied"); !status.ok())
    return status;
  if (auto status = assign(&dimension_applied, 8, "max-canvas-dimension-applied"); !status.ok())
    return status;
  if (provenance.source_canvas_width == 0 || provenance.source_canvas_height == 0 || provenance.canvas_width == 0 ||
      provenance.canvas_height == 0 || provenance.source_canvas_width < provenance.canvas_width ||
      provenance.source_canvas_height < provenance.canvas_height || width_applied > 1 || dimension_applied > 1 ||
      (width_applied != 0 && provenance.max_output_width == 0) ||
      (dimension_applied != 0 && provenance.max_canvas_dimension == 0)) {
    return absl::FailedPreconditionError("Invalid canvas provenance dimensions or constraints");
  }
  provenance.max_output_width_applied = width_applied != 0;
  provenance.max_canvas_dimension_applied = dimension_applied != 0;
  return provenance;
}

long double constrained_scale(const CanvasProvenance& provenance, size_t width, size_t dimension) {
  long double scale = 1.0L;
  if (width > 0 && provenance.source_canvas_width > width) {
    scale = std::min(scale, static_cast<long double>(width) / provenance.source_canvas_width);
  }
  const size_t longest = std::max(provenance.source_canvas_width, provenance.source_canvas_height);
  if (dimension > 0 && longest > dimension)
    scale = std::min(scale, static_cast<long double>(dimension) / longest);
  return scale;
}

bool artifacts_are_compatible(
    const CanvasProvenance& provenance,
    const CanvasSize& canvas,
    size_t max_output_width,
    size_t max_canvas_dimension) {
  if (provenance.canvas_width != canvas.width || provenance.canvas_height != canvas.height ||
      (max_output_width > 0 && canvas.width > max_output_width) ||
      (max_canvas_dimension > 0 && (canvas.width > max_canvas_dimension || canvas.height > max_canvas_dimension))) {
    return false;
  }
  if (provenance.max_output_width_applied || provenance.max_canvas_dimension_applied) {
    constexpr long double kScaleTolerance = 1e-15L;
    return std::abs(
               constrained_scale(provenance, provenance.max_output_width, provenance.max_canvas_dimension) -
               constrained_scale(provenance, max_output_width, max_canvas_dimension)) <= kScaleTolerance;
  }
  return true;
}

bool is_stitch_artifact_name(const std::string& name) {
  return std::find(kArtifacts.begin(), kArtifacts.end(), name) != kArtifacts.end();
}

bool is_stitch_partial_artifact_name(const std::string& name) {
  for (const std::string& artifact : kArtifacts) {
    if (name == "." + artifact + ".hstream-partial")
      return true;
  }
  return false;
}

absl::StatusOr<std::vector<fs::directory_entry>> stitch_directory_entries(
    const fs::path& directory,
    const std::string& description,
    size_t maximum_entries = 4096) {
  std::error_code error;
  fs::directory_iterator iterator(directory, error);
  if (error)
    return absl::InternalError("Unable to inspect " + description + ": " + error.message());
  std::vector<fs::directory_entry> entries;
  const fs::directory_iterator end;
  while (iterator != end) {
    if (entries.size() >= maximum_entries)
      return absl::ResourceExhaustedError("Too many entries while inspecting " + description);
    entries.push_back(*iterator);
    iterator.increment(error);
    if (error)
      return absl::InternalError("Unable to inspect " + description + ": " + error.message());
  }
  return entries;
}

enum class StitchStatIdentityFormat {
  kLegacyGeneration,
  kBinding,
  kPortableMetadata,
};

absl::StatusOr<std::string> stitch_artifact_stat_id(const fs::path& game_dir, StitchStatIdentityFormat format) {
  std::ostringstream generation;
  const auto append = [&](const char* name, bool required) -> absl::Status {
    struct stat metadata{};
    const fs::path path = game_dir / name;
    if (::stat(path.c_str(), &metadata) != 0) {
      if (!required && errno == ENOENT)
        return absl::OkStatus();
      return absl::NotFoundError("Hugin generation artifact is missing: " + path.string());
    }
    if (!S_ISREG(metadata.st_mode))
      return absl::NotFoundError("Hugin generation artifact is invalid: " + path.string());
    generation << name << ':';
    if (format != StitchStatIdentityFormat::kPortableMetadata) {
      generation << static_cast<uint64_t>(metadata.st_dev) << ':' << static_cast<uint64_t>(metadata.st_ino) << ':';
    }
    generation << static_cast<uint64_t>(metadata.st_size) << ':' << metadata.st_mtim.tv_sec << ':'
               << metadata.st_mtim.tv_nsec;
    if (format == StitchStatIdentityFormat::kBinding)
      generation << ':' << metadata.st_ctim.tv_sec << ':' << metadata.st_ctim.tv_nsec;
    generation << '\n';
    return absl::OkStatus();
  };
  for (const char* name : kGenerationArtifacts) {
    auto status = append(name, true);
    if (!status.ok())
      return status;
  }
  auto status = append(kStitchCanvasProvenanceArtifact, false);
  if (!status.ok())
    return status;
  return generation.str();
}

struct ParsedStitchGenerationIdentity {
  size_t version{0};
  std::string logical_id;
  std::string bindings;
  std::string fingerprint;
};

absl::StatusOr<std::string> read_stitch_generation_artifact(const fs::path& path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0) {
    if (errno == ENOENT)
      return absl::NotFoundError("Hugin generation identity is missing: " + path.string());
    return absl::FailedPreconditionError("Unable to open Hugin generation identity: " + path.string());
  }
  struct CloseDescriptor {
    int descriptor;
    ~CloseDescriptor() {
      ::close(descriptor);
    }
  } close{descriptor};
  struct stat metadata{};
  constexpr size_t kMaximumGenerationIdentityBytes = 16 * 1024;
  if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size <= 0 ||
      static_cast<uint64_t>(metadata.st_size) > kMaximumGenerationIdentityBytes) {
    return absl::FailedPreconditionError("Invalid Hugin generation identity artifact");
  }
  std::string contents(static_cast<size_t>(metadata.st_size), '\0');
  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t count = ::read(descriptor, contents.data() + offset, contents.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return absl::FailedPreconditionError("Unable to read Hugin generation identity artifact");
    offset += static_cast<size_t>(count);
  }
  return contents;
}

absl::StatusOr<size_t> parse_generation_identity_size(const std::string& line, const char* prefix) {
  const size_t prefix_size = std::strlen(prefix);
  if (line.rfind(prefix, 0) != 0 || line.size() == prefix_size)
    return absl::FailedPreconditionError("Invalid Hugin generation identity size");
  size_t value = 0;
  const auto parsed = std::from_chars(line.data() + prefix_size, line.data() + line.size(), value);
  if (parsed.ec != std::errc() || parsed.ptr != line.data() + line.size())
    return absl::FailedPreconditionError("Invalid Hugin generation identity size");
  return value;
}

absl::StatusOr<ParsedStitchGenerationIdentity> parse_stitch_generation_identity(const std::string& identity) {
  constexpr char kVersion2Header[] = "version=2\n";
  constexpr char kVersion3Header[] = "version=3\n";
  const bool version2 = identity.rfind(kVersion2Header, 0) == 0;
  const bool version3 = identity.rfind(kVersion3Header, 0) == 0;
  if (!version2 && !version3)
    return absl::FailedPreconditionError("Invalid Hugin generation identity version");
  const size_t header_size = version2 ? sizeof(kVersion2Header) - 1 : sizeof(kVersion3Header) - 1;
  const size_t second_end = identity.find('\n', header_size);
  if (second_end == std::string::npos)
    return absl::FailedPreconditionError("Truncated Hugin generation identity");
  auto logical_size =
      parse_generation_identity_size(identity.substr(header_size, second_end - header_size), "logical-size=");
  if (!logical_size.ok())
    return logical_size.status();
  const size_t third_end = identity.find('\n', second_end + 1);
  if (third_end == std::string::npos)
    return absl::FailedPreconditionError("Truncated Hugin generation identity bindings");
  auto bindings_size =
      parse_generation_identity_size(identity.substr(second_end + 1, third_end - second_end - 1), "bindings-size=");
  if (!bindings_size.ok())
    return bindings_size.status();
  size_t payload_offset = third_end + 1;
  size_t fingerprint_size = 0;
  if (version3) {
    const size_t fourth_end = identity.find('\n', payload_offset);
    if (fourth_end == std::string::npos)
      return absl::FailedPreconditionError("Truncated Hugin generation identity fingerprint");
    auto parsed_fingerprint_size = parse_generation_identity_size(
        identity.substr(payload_offset, fourth_end - payload_offset), "fingerprint-size=");
    if (!parsed_fingerprint_size.ok())
      return parsed_fingerprint_size.status();
    fingerprint_size = *parsed_fingerprint_size;
    payload_offset = fourth_end + 1;
  }
  if (*logical_size > identity.size() - payload_offset ||
      *bindings_size > identity.size() - payload_offset - *logical_size ||
      fingerprint_size != identity.size() - payload_offset - *logical_size - *bindings_size) {
    return absl::FailedPreconditionError("Invalid Hugin generation identity payload");
  }
  const std::string fingerprint = identity.substr(payload_offset + *logical_size + *bindings_size, fingerprint_size);
  if (version3 &&
      (fingerprint.size() != 64 || !std::all_of(fingerprint.begin(), fingerprint.end(), [](unsigned char value) {
         return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
       }))) {
    return absl::FailedPreconditionError("Invalid Hugin generation identity fingerprint");
  }
  return ParsedStitchGenerationIdentity{
      .version = version2 ? 2U : 3U,
      .logical_id = identity.substr(payload_offset, *logical_size),
      .bindings = identity.substr(payload_offset + *logical_size, *bindings_size),
      .fingerprint = fingerprint,
  };
}

std::string serialize_stitch_generation_identity(
    const std::string& logical_id,
    const std::string& bindings,
    const std::string& fingerprint) {
  std::ostringstream identity;
  identity << "version=3\nlogical-size=" << logical_id.size() << "\nbindings-size=" << bindings.size()
           << "\nfingerprint-size=" << fingerprint.size() << '\n'
           << logical_id << bindings << fingerprint;
  return identity.str();
}

std::string legacy_v2_mismatched_stitch_generation_id(
    const std::string& logical_id,
    const std::string& current_bindings) {
  std::ostringstream generation;
  generation << "hstream-stitch-generation-mismatch-v1\nlogical-bytes=" << logical_id.size() << '\n'
             << logical_id << "bindings-bytes=" << current_bindings.size() << '\n'
             << current_bindings;
  return generation.str();
}

absl::StatusOr<std::string> logical_stitch_generation_id_v1(
    const std::string& identity,
    const std::string& current_metadata) {
  constexpr char kHeader[] = "version=1\n";
  if (identity.rfind(kHeader, 0) != 0)
    return absl::FailedPreconditionError("Invalid Hugin generation identity version");
  const size_t second_end = identity.find('\n', sizeof(kHeader) - 1);
  if (second_end == std::string::npos)
    return absl::FailedPreconditionError("Truncated Hugin generation identity");
  const std::string second = identity.substr(sizeof(kHeader) - 1, second_end - (sizeof(kHeader) - 1));
  if (second.rfind("token=", 0) == 0) {
    const std::string token = second.substr(std::strlen("token="));
    if (token.size() != 64 || second_end + 1 != identity.size() ||
        !std::all_of(token.begin(), token.end(), [](unsigned char value) {
          return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        })) {
      return absl::FailedPreconditionError("Invalid Hugin generation token");
    }
  } else {
    auto legacy_size = parse_generation_identity_size(second, "legacy-size=");
    if (!legacy_size.ok())
      return legacy_size.status();
    const size_t third_end = identity.find('\n', second_end + 1);
    if (third_end == std::string::npos)
      return absl::FailedPreconditionError("Truncated Hugin legacy generation identity");
    auto metadata_size =
        parse_generation_identity_size(identity.substr(second_end + 1, third_end - second_end - 1), "metadata-size=");
    if (!metadata_size.ok())
      return metadata_size.status();
    const size_t payload_offset = third_end + 1;
    if (*legacy_size > identity.size() - payload_offset ||
        *metadata_size != identity.size() - payload_offset - *legacy_size) {
      return absl::FailedPreconditionError("Invalid Hugin legacy generation identity payload");
    }
    const std::string legacy = identity.substr(payload_offset, *legacy_size);
    const std::string recorded_metadata = identity.substr(payload_offset + *legacy_size, *metadata_size);
    if (recorded_metadata == current_metadata)
      return legacy;
  }
  std::ostringstream generation;
  generation << "hstream-stitch-generation-v1\nidentity-bytes=" << identity.size() << '\n'
             << identity << "metadata-bytes=" << current_metadata.size() << '\n'
             << current_metadata;
  return generation.str();
}

absl::StatusOr<std::string> random_stitch_logical_generation_id() {
  std::array<unsigned char, 32> bytes{};
  try {
    std::random_device source;
    for (unsigned char& byte : bytes)
      byte = static_cast<unsigned char>(source());
  } catch (const std::exception& exception) {
    return absl::InternalError("Unable to create Hugin generation token: " + std::string(exception.what()));
  }
  std::ostringstream identity;
  identity << "hstream-stitch-generation-v2\ntoken=" << std::hex << std::setfill('0');
  for (unsigned char byte : bytes)
    identity << std::setw(2) << static_cast<unsigned>(byte);
  identity << '\n';
  return identity.str();
}

bool stitch_stat_matches(const struct stat& before, const struct stat& after) {
  return before.st_dev == after.st_dev && before.st_ino == after.st_ino && before.st_size == after.st_size &&
      before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
      before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}

struct PinnedStitchArtifact {
  std::string name;
  int descriptor{-1};
  struct stat metadata{};

  PinnedStitchArtifact(std::string name_value, int descriptor_value, const struct stat& metadata_value)
      : name(std::move(name_value)), descriptor(descriptor_value), metadata(metadata_value) {}
  ~PinnedStitchArtifact() {
    if (descriptor >= 0)
      ::close(descriptor);
  }
  PinnedStitchArtifact(PinnedStitchArtifact&& other) noexcept
      : name(std::move(other.name)), descriptor(other.descriptor), metadata(other.metadata) {
    other.descriptor = -1;
  }
  PinnedStitchArtifact& operator=(PinnedStitchArtifact&& other) noexcept {
    if (this == &other)
      return *this;
    if (descriptor >= 0)
      ::close(descriptor);
    name = std::move(other.name);
    descriptor = other.descriptor;
    metadata = other.metadata;
    other.descriptor = -1;
    return *this;
  }
  PinnedStitchArtifact(const PinnedStitchArtifact&) = delete;
  PinnedStitchArtifact& operator=(const PinnedStitchArtifact&) = delete;
};

struct StitchArtifactFingerprint {
  std::string value;
  std::vector<PinnedStitchArtifact> artifacts;
};

bool stitch_file_metadata_matches_without_ctime(const struct stat& before, const struct stat& after) {
  return before.st_dev == after.st_dev && before.st_ino == after.st_ino && before.st_size == after.st_size &&
      before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
}

absl::StatusOr<StitchArtifactFingerprint> stitch_artifact_fingerprint_impl(
    const fs::path& game_dir,
    bool retain_descriptors) {
  StitchArtifactFingerprint result;
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (context == nullptr || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
    if (context != nullptr)
      EVP_MD_CTX_free(context);
    return absl::InternalError("Unable to initialize Hugin artifact fingerprint");
  }
  struct FreeDigestContext {
    EVP_MD_CTX* context;
    ~FreeDigestContext() {
      EVP_MD_CTX_free(context);
    }
  } free_context{context};

  const auto append = [&](const char* name, bool required) -> absl::Status {
    const fs::path path = game_dir / name;
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
      if (!required && errno == ENOENT) {
        const char missing = '0';
        if (EVP_DigestUpdate(context, name, std::strlen(name) + 1) != 1 ||
            EVP_DigestUpdate(context, &missing, sizeof(missing)) != 1) {
          return absl::InternalError("Unable to fingerprint absent optional Hugin artifact");
        }
        if (retain_descriptors) {
          struct stat missing{};
          result.artifacts.emplace_back(name, -1, missing);
        }
        return absl::OkStatus();
      }
      return absl::NotFoundError("Hugin generation artifact is missing while fingerprinting: " + path.string());
    }
    struct CloseDescriptor {
      int descriptor;
      ~CloseDescriptor() {
        if (descriptor >= 0)
          ::close(descriptor);
      }
    } close{descriptor};
    struct stat before{};
    if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size <= 0)
      return absl::FailedPreconditionError("Invalid Hugin artifact while fingerprinting: " + path.string());
    uint64_t maximum_bytes = maximum_stitch_artifact_bytes(name);
    if (maximum_bytes == 0 || static_cast<uint64_t>(before.st_size) > maximum_bytes) {
      return absl::FailedPreconditionError("Oversized Hugin artifact while fingerprinting: " + path.string());
    }
    if (fs::path(name).extension() == ".tif") {
      auto tiff_maximum = maximum_open_tiff_artifact_bytes(descriptor, path);
      if (!tiff_maximum.ok())
        return absl::FailedPreconditionError(
            "Invalid bounded TIFF artifact while fingerprinting: " + path.string() + ": " +
            std::string(tiff_maximum.status().message()));
      maximum_bytes = *tiff_maximum;
    }
    if (static_cast<uint64_t>(before.st_size) > maximum_bytes) {
      return absl::FailedPreconditionError("Oversized Hugin artifact while fingerprinting: " + path.string());
    }
    const char present = '1';
    const std::string size = std::to_string(static_cast<uint64_t>(before.st_size));
    if (EVP_DigestUpdate(context, name, std::strlen(name) + 1) != 1 ||
        EVP_DigestUpdate(context, &present, sizeof(present)) != 1 ||
        EVP_DigestUpdate(context, size.data(), size.size() + 1) != 1) {
      return absl::InternalError("Unable to update Hugin artifact fingerprint metadata");
    }
    std::vector<unsigned char> buffer(1024 * 1024);
    uint64_t offset = 0;
    while (offset < static_cast<uint64_t>(before.st_size)) {
      const size_t requested =
          static_cast<size_t>(std::min<uint64_t>(buffer.size(), static_cast<uint64_t>(before.st_size) - offset));
      const ssize_t count = ::pread(descriptor, buffer.data(), requested, static_cast<off_t>(offset));
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        return absl::InternalError("Unable to read Hugin artifact while fingerprinting: " + path.string());
      if (EVP_DigestUpdate(context, buffer.data(), static_cast<size_t>(count)) != 1)
        return absl::InternalError("Unable to update Hugin artifact fingerprint payload");
      offset += static_cast<uint64_t>(count);
    }
    struct stat after{};
    if (::fstat(descriptor, &after) != 0 || !stitch_stat_matches(before, after))
      return absl::AbortedError("Hugin artifact changed while fingerprinting: " + path.string());
    if (retain_descriptors) {
      result.artifacts.emplace_back(name, descriptor, before);
      close.descriptor = -1;
    }
    return absl::OkStatus();
  };

  for (const char* name : kGenerationArtifacts) {
    auto status = append(name, true);
    if (!status.ok())
      return status;
  }
  auto status = append(kStitchCanvasProvenanceArtifact, false);
  if (!status.ok())
    return status;
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned length = 0;
  if (EVP_DigestFinal_ex(context, digest.data(), &length) != 1 || length != 32)
    return absl::InternalError("Unable to finalize Hugin artifact fingerprint");
  std::ostringstream value;
  value << std::hex << std::setfill('0');
  for (unsigned index = 0; index < length; ++index)
    value << std::setw(2) << static_cast<unsigned>(digest[index]);
  result.value = value.str();
  return result;
}

absl::StatusOr<std::string> stitch_artifact_fingerprint(const fs::path& game_dir) {
  auto fingerprint = stitch_artifact_fingerprint_impl(game_dir, /*retain_descriptors=*/false);
  if (!fingerprint.ok())
    return fingerprint.status();
  return std::move(fingerprint->value);
}

bool stitch_filesystem_has_unreliable_metadata(const fs::path& game_dir) {
  if (const char* value = std::getenv("HM_TEST_STITCH_UNRELIABLE_METADATA");
      value != nullptr && std::string(value) == "1") {
    return true;
  }
  struct statfs filesystem{};
  if (::statfs(game_dir.c_str(), &filesystem) != 0)
    return true;
  switch (static_cast<unsigned long>(filesystem.f_type)) {
    case kFuseSuperMagic:
    case kMsDosSuperMagic:
    case kExFatSuperMagic:
    case kNfsSuperMagic:
    case kSmbSuperMagic:
    case kCifsSuperMagic:
    case kSmb2SuperMagic:
      return true;
    default:
      return false;
  }
}

std::string content_scoped_stitch_generation_id(const std::string& fingerprint) {
  return "hstream-stitch-content-v1\nsha256=" + fingerprint + "\n";
}

absl::StatusOr<std::string> read_stitch_manifest(const fs::path& path, const char* description) {
  return read_bounded_stitch_file(path, 4 * 1024, description);
}

absl::StatusOr<std::string> read_stitch_transaction_state(const fs::path& transaction) {
  const fs::path state_path = transaction / "state";
  std::error_code error;
  const fs::file_status state_status = fs::symlink_status(state_path, error);
  if (error == std::errc::no_such_file_or_directory)
    error.clear();
  else if (error)
    return absl::InternalError("Unable to inspect stitch transaction state: " + error.message());
  if (state_status.type() == fs::file_type::not_found)
    return std::string("UNPREPARED");
  const int descriptor = ::open(state_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0)
    return absl::FailedPreconditionError("Unable to open durable stitch transaction state");
  struct CloseDescriptor {
    int descriptor;
    ~CloseDescriptor() {
      ::close(descriptor);
    }
  } close{descriptor};
  struct stat metadata{};
  if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
      metadata.st_size > 16) {
    return absl::FailedPreconditionError("Invalid durable stitch transaction state file");
  }
  std::string contents(static_cast<size_t>(metadata.st_size), '\0');
  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t count = ::read(descriptor, contents.data() + offset, contents.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return absl::FailedPreconditionError("Unable to read durable stitch transaction state");
    offset += static_cast<size_t>(count);
  }
  if (contents == "PREPARED\n")
    return std::string("PREPARED");
  if (contents == "BACKING_UP\n")
    return std::string("BACKING_UP");
  if (contents == "BACKED_UP\n")
    return std::string("BACKED_UP");
  if (contents == "LEGACY_MIGRATE\n")
    return std::string("LEGACY_MIGRATE");
  if (contents == "ROLLING_BACK\n")
    return std::string("ROLLING_BACK");
  if (contents == "RESTORED\n")
    return std::string("RESTORED");
  if (contents == "COMMITTED\n")
    return std::string("COMMITTED");
  if (contents == "ROLLED_BACK\n")
    return std::string("ROLLED_BACK");
  return absl::FailedPreconditionError("Invalid durable stitch transaction state contents");
}

absl::StatusOr<bool> has_current_stitch_transaction_protocol(const fs::path& transaction) {
  const fs::path path = transaction / "journal_version";
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0) {
    if (errno == ENOENT)
      return false;
    return absl::FailedPreconditionError("Unable to open stitch transaction journal version");
  }
  struct stat metadata{};
  if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size != 2) {
    ::close(descriptor);
    return absl::FailedPreconditionError("Invalid stitch transaction journal version file");
  }
  char contents[2] = {};
  size_t offset = 0;
  while (offset < sizeof(contents)) {
    const ssize_t count = ::read(descriptor, contents + offset, sizeof(contents) - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      ::close(descriptor);
      return absl::FailedPreconditionError("Unable to read stitch transaction journal version");
    }
    offset += static_cast<size_t>(count);
  }
  ::close(descriptor);
  if (contents[0] != '2' || contents[1] != '\n')
    return absl::FailedPreconditionError("Invalid stitch transaction journal version contents");
  return true;
}

} // namespace

struct PreparedStitchGenerationPublication::Impl {
  std::string logical_id;
  std::string fingerprint;
  std::vector<PinnedStitchArtifact> artifacts;
};

PreparedStitchGenerationPublication::PreparedStitchGenerationPublication(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

PreparedStitchGenerationPublication::~PreparedStitchGenerationPublication() = default;
PreparedStitchGenerationPublication::PreparedStitchGenerationPublication(
    PreparedStitchGenerationPublication&& other) noexcept = default;
PreparedStitchGenerationPublication& PreparedStitchGenerationPublication::operator=(
    PreparedStitchGenerationPublication&& other) noexcept = default;

const std::vector<std::string>& required_stitch_artifact_names() {
  return kRequiredArtifacts;
}

const std::vector<std::string>& stitch_artifact_names() {
  return kArtifacts;
}

absl::Status validate_stitch_generation_artifact_bounds_locked(const fs::path& game_dir) {
  for (const char* name : kGenerationArtifacts) {
    const absl::Status status = validate_stitch_artifact_bounds(game_dir, name, /*required=*/true);
    if (!status.ok())
      return status;
  }
  return validate_stitch_artifact_bounds(game_dir, kStitchCanvasProvenanceArtifact, /*required=*/false);
}

absl::Status validate_stitch_seam_repair_artifact_bounds_locked(const fs::path& game_dir) {
  for (const char* name : {"mapping_0000.tif", "mapping_0001.tif"}) {
    const absl::Status status = validate_stitch_artifact_bounds(game_dir, name, /*required=*/true);
    if (!status.ok())
      return status;
  }
  return validate_stitch_artifact_bounds(game_dir, "seam_file.png", /*required=*/false);
}

absl::Status fsync_stitch_path(const fs::path& path, bool directory) {
  const int flags = O_RDONLY | O_CLOEXEC | (directory ? O_DIRECTORY : 0);
  const int descriptor = ::open(path.c_str(), flags);
  if (descriptor < 0)
    return absl::InternalError("Unable to open Hugin artifact for fsync: " + path.string());
  const int result = ::fsync(descriptor);
  const std::string message = result == 0 ? std::string() : std::strerror(errno);
  ::close(descriptor);
  if (result != 0)
    return absl::InternalError("Unable to fsync Hugin artifact " + path.string() + ": " + message);
  return absl::OkStatus();
}

absl::Status clone_or_copy_stitch_rollback_file(const fs::path& source, const fs::path& destination) {
  const bool force_portable_fallback = [] {
    const char* value = std::getenv("HM_TEST_STITCH_DISABLE_LINK_CLONE");
    return value != nullptr && std::string(value) == "1";
  }();
  const uint64_t maximum_bytes = maximum_stitch_artifact_bytes(source.filename().string());
  if (maximum_bytes == 0)
    return absl::FailedPreconditionError("Unrecognized stitch rollback artifact: " + source.string());
  return snapshot_regular_file_for_rollback(
      source, destination, force_portable_fallback, static_cast<size_t>(maximum_bytes));
}

absl::StatusOr<bool> regular_stitch_file_exists_no_follow(const fs::path& path) {
  struct stat metadata{};
  if (::lstat(path.c_str(), &metadata) == 0) {
    if (!S_ISREG(metadata.st_mode))
      return absl::FailedPreconditionError("Stitch artifact is not a regular file: " + path.string());
    return true;
  }
  if (errno == ENOENT)
    return false;
  return absl::InternalError("Unable to inspect stitch artifact " + path.string() + ": " + std::strerror(errno));
}

absl::Status write_stitch_transaction_file(const fs::path& path, const std::string& contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    return absl::InternalError("Unable to write Hugin file: " + path.string());
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.flush();
  if (!output)
    return absl::InternalError("Failed writing Hugin file: " + path.string());
  output.close();
  return fsync_stitch_path(path);
}

absl::Status publish_stitch_file_atomically(const fs::path& path, const std::string& contents) {
  mode_t published_mode = 0644;
  struct stat existing{};
  if (::lstat(path.c_str(), &existing) == 0) {
    if (!S_ISREG(existing.st_mode))
      return absl::FailedPreconditionError("Existing Hugin publication target is not a regular file");
    published_mode = existing.st_mode & 07777;
  } else if (errno != ENOENT) {
    return absl::InternalError("Unable to inspect Hugin publication target: " + std::string(std::strerror(errno)));
  }
  std::string pattern = (path.parent_path() / ("." + path.filename().string() + "-XXXXXX")).string();
  std::vector<char> writable_pattern(pattern.begin(), pattern.end());
  writable_pattern.push_back('\0');
  const int descriptor = ::mkstemp(writable_pattern.data());
  if (descriptor < 0)
    return absl::InternalError("Unable to create temporary Hugin file: " + std::string(std::strerror(errno)));
  const fs::path temporary(writable_pattern.data());
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      if (path.empty())
        return;
      std::error_code ignored;
      fs::remove(path, ignored);
    }
  } cleanup{temporary};

  const bool inject_unsupported_chmod = [] {
    const char* value = std::getenv("HM_TEST_STITCH_FCHMOD_UNSUPPORTED");
    return value != nullptr && std::string(value) == "1";
  }();
  const int chmod_result = inject_unsupported_chmod ? (errno = EOPNOTSUPP, -1) : ::fchmod(descriptor, published_mode);
  if (chmod_result != 0 && errno != EOPNOTSUPP && errno != ENOTSUP && errno != EPERM) {
    const int saved_errno = errno;
    ::close(descriptor);
    return absl::InternalError(
        "Unable to set temporary Hugin file permissions: " + std::string(std::strerror(saved_errno)));
  }
  struct stat temporary_metadata{};
  if (::fstat(descriptor, &temporary_metadata) != 0 || !S_ISREG(temporary_metadata.st_mode)) {
    const int saved_errno = errno;
    ::close(descriptor);
    return absl::InternalError("Unable to validate temporary Hugin file: " + std::string(std::strerror(saved_errno)));
  }

  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t count = ::write(descriptor, contents.data() + offset, contents.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      const int saved_errno = errno;
      ::close(descriptor);
      return absl::InternalError("Unable to write temporary Hugin file: " + std::string(std::strerror(saved_errno)));
    }
    offset += static_cast<size_t>(count);
  }
  if (::fsync(descriptor) != 0) {
    const int saved_errno = errno;
    ::close(descriptor);
    return absl::InternalError("Unable to fsync temporary Hugin file: " + std::string(std::strerror(saved_errno)));
  }
  if (::close(descriptor) != 0)
    return absl::InternalError("Unable to close temporary Hugin file: " + std::string(std::strerror(errno)));
  std::error_code error;
  fs::rename(temporary, path, error);
  if (error)
    return absl::InternalError("Unable to atomically publish Hugin file: " + error.message());
  cleanup.path.clear();
  return fsync_stitch_path(path.parent_path(), true);
}

absl::StatusOr<PreparedStitchGenerationPublication> prepare_stitch_generation_publication(
    const fs::path& staging,
    const fs::path& game_dir) {
  const fs::path committed_identity = game_dir / kStitchGenerationArtifact;
  std::error_code error;
  const bool identity_exists = fs::exists(committed_identity, error);
  if (error)
    return absl::InternalError("Unable to inspect Hugin generation identity: " + error.message());
  if (identity_exists) {
    if (!fs::is_regular_file(committed_identity, error) || error)
      return absl::FailedPreconditionError("Existing Hugin generation identity is not a regular file");
    auto current_bindings = stitch_artifact_stat_id(game_dir, StitchStatIdentityFormat::kBinding);
    if (current_bindings.ok()) {
      auto generation = stitch_artifact_generation_id_locked(game_dir);
      if (!generation.ok())
        return generation.status();
    } else if (!absl::IsNotFound(current_bindings.status())) {
      return current_bindings.status();
    }
  } else {
    auto legacy = stitch_artifact_stat_id(game_dir, StitchStatIdentityFormat::kLegacyGeneration);
    if (legacy.ok()) {
      auto bindings = stitch_artifact_stat_id(game_dir, StitchStatIdentityFormat::kBinding);
      if (!bindings.ok())
        return bindings.status();
      auto fingerprint = stitch_artifact_fingerprint(game_dir);
      if (!fingerprint.ok())
        return fingerprint.status();
      const fs::path adoption = staging / ".previous-stitching-generation-id";
      auto status = write_stitch_transaction_file(
          adoption, serialize_stitch_generation_identity(*legacy, *bindings, *fingerprint));
      if (!status.ok())
        return status;
      fs::rename(adoption, committed_identity, error);
      if (error)
        return absl::InternalError("Unable to adopt legacy Hugin generation identity: " + error.message());
      status = fsync_stitch_path(game_dir, true);
      if (!status.ok())
        return status;
    } else if (!absl::IsNotFound(legacy.status())) {
      return legacy.status();
    }
  }
  auto logical_id = random_stitch_logical_generation_id();
  if (!logical_id.ok())
    return logical_id.status();
  auto bindings = stitch_artifact_stat_id(staging, StitchStatIdentityFormat::kBinding);
  if (!bindings.ok())
    return bindings.status();
  auto fingerprint = stitch_artifact_fingerprint_impl(staging, /*retain_descriptors=*/true);
  if (!fingerprint.ok())
    return fingerprint.status();
  auto status = write_stitch_transaction_file(
      staging / kStitchGenerationArtifact,
      serialize_stitch_generation_identity(*logical_id, *bindings, fingerprint->value));
  if (!status.ok())
    return status;
  auto prepared = std::make_unique<PreparedStitchGenerationPublication::Impl>();
  prepared->logical_id = std::move(*logical_id);
  prepared->fingerprint = std::move(fingerprint->value);
  prepared->artifacts = std::move(fingerprint->artifacts);
  return PreparedStitchGenerationPublication(std::move(prepared));
}

absl::Status rebind_published_stitch_generation_artifact(
    const PreparedStitchGenerationPublication& prepared,
    const fs::path& game_dir) {
  if (!prepared.impl_)
    return absl::InvalidArgumentError("Prepared Hugin publication is empty");
  std::ostringstream bindings;
  for (const PinnedStitchArtifact& artifact : prepared.impl_->artifacts) {
    const fs::path path = game_dir / artifact.name;
    if (artifact.descriptor < 0) {
      struct stat metadata{};
      if (::lstat(path.c_str(), &metadata) == 0)
        return absl::AbortedError("Absent optional Hugin artifact appeared during publication: " + path.string());
      if (errno != ENOENT) {
        return absl::InternalError(
            "Unable to inspect optional Hugin artifact after publication: " + std::string(std::strerror(errno)));
      }
      continue;
    }
    struct stat pinned{};
    if (::fstat(artifact.descriptor, &pinned) != 0 || !stitch_stat_matches(artifact.metadata, pinned)) {
      return absl::AbortedError("Hugin artifact changed during publication: " + path.string());
    }
    struct stat published{};
    if (::lstat(path.c_str(), &published) != 0) {
      return absl::AbortedError("Hugin artifact is missing after publication: " + path.string());
    }
    if (!S_ISREG(published.st_mode) || !stitch_stat_matches(artifact.metadata, published)) {
      return absl::AbortedError("Published Hugin artifact no longer matches its staged file: " + path.string());
    }
    bindings << artifact.name << ':' << static_cast<uint64_t>(pinned.st_dev) << ':'
             << static_cast<uint64_t>(pinned.st_ino) << ':' << static_cast<uint64_t>(pinned.st_size) << ':'
             << pinned.st_mtim.tv_sec << ':' << pinned.st_mtim.tv_nsec << ':' << pinned.st_ctim.tv_sec << ':'
             << pinned.st_ctim.tv_nsec << '\n';
  }
  return publish_stitch_file_atomically(
      game_dir / kStitchGenerationArtifact,
      serialize_stitch_generation_identity(prepared.impl_->logical_id, bindings.str(), prepared.impl_->fingerprint));
}

absl::Status validate_prepared_stitch_generation_artifact(
    const PreparedStitchGenerationPublication& prepared,
    const fs::path& staging,
    const std::string& name) {
  if (!prepared.impl_)
    return absl::InvalidArgumentError("Prepared Hugin publication is empty");
  const auto artifact = std::find_if(
      prepared.impl_->artifacts.begin(), prepared.impl_->artifacts.end(), [&](const PinnedStitchArtifact& candidate) {
        return candidate.name == name;
      });
  if (artifact == prepared.impl_->artifacts.end())
    return absl::OkStatus();
  if (artifact->descriptor < 0)
    return absl::AbortedError("Optional Hugin artifact appeared after publication preparation: " + name);
  struct stat pinned{};
  struct stat staged{};
  if (::fstat(artifact->descriptor, &pinned) != 0 || ::lstat((staging / name).c_str(), &staged) != 0 ||
      !stitch_stat_matches(artifact->metadata, pinned) || !stitch_stat_matches(pinned, staged)) {
    return absl::AbortedError("Hugin artifact changed before publication: " + (staging / name).string());
  }
  return absl::OkStatus();
}

absl::Status record_published_stitch_generation_artifact(
    PreparedStitchGenerationPublication& prepared,
    const fs::path& game_dir,
    const std::string& name) {
  if (!prepared.impl_)
    return absl::InvalidArgumentError("Prepared Hugin publication is empty");
  const auto artifact = std::find_if(
      prepared.impl_->artifacts.begin(), prepared.impl_->artifacts.end(), [&](const PinnedStitchArtifact& candidate) {
        return candidate.name == name;
      });
  if (artifact == prepared.impl_->artifacts.end())
    return absl::OkStatus();
  if (artifact->descriptor < 0)
    return absl::AbortedError("Optional Hugin artifact appeared during publication: " + name);
  struct stat pinned{};
  struct stat published{};
  if (::fstat(artifact->descriptor, &pinned) != 0 || ::lstat((game_dir / name).c_str(), &published) != 0 ||
      !S_ISREG(published.st_mode) || !stitch_file_metadata_matches_without_ctime(artifact->metadata, pinned) ||
      !stitch_stat_matches(pinned, published)) {
    return absl::AbortedError("Hugin artifact changed while being published: " + (game_dir / name).string());
  }
  artifact->metadata = pinned;
  return absl::OkStatus();
}

absl::Status rebind_stitch_generation_artifact(const fs::path& transaction, const fs::path& game_dir) {
  (void)transaction;
  const fs::path identity_path = game_dir / kStitchGenerationArtifact;
  std::error_code error;
  const bool exists = fs::exists(identity_path, error);
  if (error)
    return absl::InternalError("Unable to inspect restored Hugin generation identity: " + error.message());
  if (!exists)
    return absl::OkStatus();
  auto identity = read_stitch_generation_artifact(identity_path);
  if (!identity.ok())
    return identity.status();
  auto bindings = stitch_artifact_stat_id(game_dir, StitchStatIdentityFormat::kBinding);
  if (!bindings.ok())
    return bindings.status();
  std::string logical_id;
  std::string recorded_fingerprint;
  if (identity->rfind("version=1\n", 0) == 0) {
    auto metadata = stitch_artifact_stat_id(game_dir, StitchStatIdentityFormat::kPortableMetadata);
    if (!metadata.ok())
      return metadata.status();
    auto legacy_logical_id = logical_stitch_generation_id_v1(*identity, *metadata);
    if (!legacy_logical_id.ok())
      return legacy_logical_id.status();
    logical_id = std::move(*legacy_logical_id);
  } else {
    auto parsed = parse_stitch_generation_identity(*identity);
    if (!parsed.ok())
      return parsed.status();
    logical_id = std::move(parsed->logical_id);
    recorded_fingerprint = std::move(parsed->fingerprint);
  }
  auto fingerprint = stitch_artifact_fingerprint(game_dir);
  if (!fingerprint.ok())
    return fingerprint.status();
  auto verified_bindings = stitch_artifact_stat_id(game_dir, StitchStatIdentityFormat::kBinding);
  if (!verified_bindings.ok())
    return verified_bindings.status();
  if (*verified_bindings != *bindings)
    return absl::AbortedError("Hugin artifacts changed while rebinding their generation identity");
  if (!recorded_fingerprint.empty() && recorded_fingerprint != *fingerprint) {
    auto replacement_logical_id = random_stitch_logical_generation_id();
    if (!replacement_logical_id.ok())
      return replacement_logical_id.status();
    logical_id = std::move(*replacement_logical_id);
  }
  return publish_stitch_file_atomically(
      identity_path, serialize_stitch_generation_identity(logical_id, *bindings, *fingerprint));
}

absl::Status mark_stitch_transaction_rolled_back(const fs::path& transaction) {
  return publish_transaction_state(transaction, "ROLLED_BACK\n");
}

absl::Status recover_stitch_transactions_locked(const fs::path& root) {
  auto scan_required = transaction_recovery_scan_required(root, TransactionJournalKind::kStitch);
  if (!scan_required.ok())
    return scan_required.status();
  if (!*scan_required)
    return absl::OkStatus();
  std::error_code error;
  bool recovered = false;
  auto opened_root = PinnedDirectory::Open(root, "stitch transaction root");
  if (!opened_root.ok())
    return opened_root.status();
  PinnedDirectory root_directory = std::move(*opened_root);
  auto root_entries = stitch_directory_entries(root_directory.path(), "stitch transactions");
  if (!root_entries.ok())
    return root_entries.status();
  for (const auto& entry : *root_entries) {
    const std::string directory_name = entry.path().filename().string();
    if (directory_name.rfind(kStitchTransactionPrefix, 0) != 0 || directory_name == ".hstream-stitch-journal-v1" ||
        directory_name == ".hstream-stitch-recovery-pending")
      continue;
    auto opened_transaction = root_directory.OpenChild(directory_name, "stitch transaction directory");
    if (!opened_transaction.ok())
      return opened_transaction.status();
    if (!opened_transaction->has_value())
      continue;
    PinnedDirectory transaction_directory = std::move(**opened_transaction);
    recovered = true;
    const fs::path transaction = transaction_directory.path();
    auto state = read_stitch_transaction_state(transaction);
    if (!state.ok())
      return state.status();
    if (*state == "RESTORED") {
      auto current_protocol = has_current_stitch_transaction_protocol(transaction);
      if (!current_protocol.ok())
        return current_protocol.status();
      if (!*current_protocol)
        return absl::FailedPreconditionError("Unversioned stitch transaction cannot claim restored artifacts");
      auto opened_previous = transaction_directory.OpenChild("previous", "stitch transaction backup directory");
      if (!opened_previous.ok())
        return opened_previous.status();
      if (opened_previous->has_value()) {
        auto cleanup = remove_pinned_directory(transaction_directory, "previous", **opened_previous);
        if (!cleanup.ok())
          return cleanup;
      }
      auto status = fsync_stitch_path(transaction, true);
      if (!status.ok())
        return status;
      status = rebind_stitch_generation_artifact(transaction, root_directory.path());
      if (!status.ok())
        return status;
      status = mark_stitch_transaction_rolled_back(transaction);
      if (!status.ok())
        return status;
    }
    if (*state == "PREPARED" || *state == "BACKING_UP" || *state == "BACKED_UP" || *state == "LEGACY_MIGRATE" ||
        *state == "ROLLING_BACK") {
      auto current_protocol = has_current_stitch_transaction_protocol(transaction);
      if (!current_protocol.ok())
        return current_protocol.status();
      if (!*current_protocol) {
        return absl::FailedPreconditionError(
            "Unversioned stitch rollback cannot prove that its backups belong to one generation");
      }
      if (*state == "LEGACY_MIGRATE")
        return absl::FailedPreconditionError("Legacy stitch migration journals cannot prove backup provenance");
      auto manifest_contents = read_stitch_manifest(transaction / "artifacts", "stitch artifact manifest");
      if (!manifest_contents.ok())
        return manifest_contents.status();
      std::istringstream manifest(*manifest_contents);
      std::set<std::string> manifested;
      for (std::string name; manifest >> name;) {
        if (fs::path(name).filename() != name || !is_stitch_artifact_name(name) || !manifested.insert(name).second)
          return absl::InvalidArgumentError("Invalid stitch transaction filename: " + name);
      }
      const std::set<std::string> current(kArtifacts.begin(), kArtifacts.end());
      if (!manifest.eof() ||
          (manifested != current && manifested != kArtifactsWithoutGeneration && manifested != kPreviousArtifacts &&
           manifested != kLegacyArtifacts)) {
        return absl::FailedPreconditionError("Prepared stitch transaction has an incomplete artifact manifest");
      }

      std::map<std::string, fs::path> backups;
      auto opened_previous = transaction_directory.OpenChild("previous", "stitch transaction backup directory");
      if (!opened_previous.ok())
        return opened_previous.status();
      if (!opened_previous->has_value())
        return absl::FailedPreconditionError("Stitch transaction backup is not a directory");
      PinnedDirectory previous_directory = std::move(**opened_previous);
      const fs::path previous = previous_directory.path();
      auto previous_entries = stitch_directory_entries(previous, "stitch transaction backup", kArtifacts.size() * 2);
      if (!previous_entries.ok())
        return previous_entries.status();
      for (const auto& old : *previous_entries) {
        const std::string old_name = old.path().filename().string();
        const bool is_regular = old.symlink_status(error).type() == fs::file_type::regular;
        if (error)
          return absl::InternalError("Unable to inspect stitch transaction backup entry: " + error.message());
        if (is_regular && is_stitch_partial_artifact_name(old_name)) {
          fs::remove(old.path(), error);
          if (error)
            return absl::InternalError("Unable to remove incomplete stitch transaction backup: " + error.message());
          continue;
        }
        if (!is_regular || !is_stitch_artifact_name(old_name) || !backups.emplace(old_name, old.path()).second) {
          return absl::InvalidArgumentError("Invalid stitch transaction backup: " + old_name);
        }
      }

      std::set<std::string> prior_artifacts;
      const fs::path prior_manifest_path = transaction / "previous_artifacts";
      const bool has_prior_manifest = fs::exists(prior_manifest_path, error);
      if (error)
        return absl::InternalError("Unable to inspect stitch transaction prior-artifact manifest: " + error.message());
      absl::Status prior_manifest_status = absl::OkStatus();
      bool prior_manifest_valid = false;
      if (has_prior_manifest) {
        auto prior_manifest_contents = read_stitch_manifest(prior_manifest_path, "prior stitch artifact manifest");
        if (!prior_manifest_contents.ok()) {
          prior_manifest_status = prior_manifest_contents.status();
        } else {
          std::istringstream prior_manifest(*prior_manifest_contents);
          for (std::string name; prior_manifest >> name;) {
            if (fs::path(name).filename() != name || manifested.count(name) == 0 ||
                !prior_artifacts.insert(name).second) {
              prior_manifest_status = absl::InvalidArgumentError("Invalid prior stitch transaction filename: " + name);
              break;
            }
          }
          if (prior_manifest_status.ok() && !prior_manifest.eof()) {
            prior_manifest_status =
                absl::FailedPreconditionError("Prepared stitch transaction has an invalid prior-artifact manifest");
          }
          prior_manifest_valid = prior_manifest_status.ok();
        }
      }

      if (*state == "PREPARED") {
        if (!prior_manifest_valid)
          return has_prior_manifest
              ? prior_manifest_status
              : absl::FailedPreconditionError("Prepared stitch transaction has no prior-artifact manifest");
        if (!backups.empty())
          return absl::FailedPreconditionError("Prepared stitch transaction unexpectedly contains backups");
        auto status = mark_stitch_transaction_rolled_back(transaction);
        if (!status.ok())
          return status;
      } else if (!prior_manifest_valid) {
        if (!has_prior_manifest)
          return absl::FailedPreconditionError("Active stitch transaction has no prior-artifact manifest");
        return prior_manifest_status;
      }

      bool backing_up_roots_complete = false;
      if (*state == "BACKING_UP") {
        backing_up_roots_complete = true;
        for (const std::string& name : prior_artifacts) {
          auto root_is_regular = regular_stitch_file_exists_no_follow(root_directory.path() / name);
          if (!root_is_regular.ok())
            return root_is_regular.status();
          backing_up_roots_complete &= *root_is_regular;
        }
        if (backing_up_roots_complete) {
          auto status = mark_stitch_transaction_rolled_back(transaction);
          if (!status.ok())
            return status;
        }
      }

      if (!backing_up_roots_complete && (*state == "BACKING_UP" || *state == "BACKED_UP" || *state == "ROLLING_BACK")) {
        // A crash between the two directory fsyncs for a cross-directory
        // rename may expose both names. Do not reject that recoverable state;
        // the private backup remains authoritative during rollback.
        for (const auto& [name, old] : backups) {
          (void)old;
          if (prior_artifacts.count(name) == 0) {
            return absl::FailedPreconditionError("Stitch transaction contains an unmanifested backup: " + name);
          }
        }
        if ((*state == "BACKED_UP" || *state == "ROLLING_BACK") && backups.size() != prior_artifacts.size()) {
          return absl::FailedPreconditionError("Stitch transaction has incomplete durable rollback artifacts");
        }
        if (*state == "BACKING_UP") {
          for (const std::string& name : prior_artifacts) {
            auto root_is_regular = regular_stitch_file_exists_no_follow(root_directory.path() / name);
            if (!root_is_regular.ok())
              return root_is_regular.status();
            if (!*root_is_regular) {
              if (backups.count(name) != 0)
                continue;
              return absl::FailedPreconditionError(
                  "Backup-in-progress stitch transaction lost prior artifact: " + name);
            }
            const auto existing_backup = backups.find(name);
            if (existing_backup != backups.end()) {
              fs::remove(existing_backup->second, error);
              if (error)
                return absl::InternalError("Unable to replace incomplete stitch backup: " + error.message());
              backups.erase(existing_backup);
            }
            auto preserve = clone_or_copy_stitch_rollback_file(root_directory.path() / name, previous / name);
            if (!preserve.ok())
              return preserve;
            backups.emplace(name, previous / name);
          }
          auto status = fsync_stitch_path(previous, true);
          if (!status.ok())
            return status;
        }

        if (backups.size() != prior_artifacts.size())
          return absl::FailedPreconditionError("Stitch transaction has incomplete durable rollback artifacts");

        std::map<std::string, fs::path> staged_restores;
        for (const std::string& name : prior_artifacts) {
          const auto backup = backups.find(name);
          if (backup == backups.end())
            return absl::FailedPreconditionError("Stitch transaction lost a durable rollback artifact: " + name);
          const fs::path staged = transaction / (".restore-" + name);
          fs::remove(staged, error);
          if (error)
            return absl::InternalError("Unable to remove stale stitch restore staging file: " + error.message());
          auto restore = clone_or_copy_stitch_rollback_file(backup->second, staged);
          if (!restore.ok())
            return restore;
          auto status = fsync_stitch_path(staged);
          if (!status.ok())
            return status;
          staged_restores.emplace(name, staged);
        }
        auto status = fsync_stitch_path(transaction, true);
        if (!status.ok())
          return status;
        if (*state != "ROLLING_BACK") {
          status = publish_transaction_state(transaction, "ROLLING_BACK\n");
          if (!status.ok())
            return status;
        }
        for (const std::string& name : manifested) {
          if (prior_artifacts.count(name) != 0)
            continue;
          fs::remove(root_directory.path() / name, error);
          if (error)
            return absl::InternalError("Unable to remove interrupted stitch artifact: " + error.message());
        }
        size_t restored = 0;
        for (const std::string& name : prior_artifacts) {
          fs::rename(staged_restores.at(name), root_directory.path() / name, error);
          if (error)
            return absl::InternalError("Unable to atomically restore interrupted stitch artifact: " + error.message());
          ++restored;
          if (const char* fail_after = std::getenv("HM_TEST_STITCH_ROLLBACK_INTERRUPT_AFTER");
              fail_after != nullptr && restored == static_cast<size_t>(std::strtoull(fail_after, nullptr, 10))) {
            return absl::InternalError("Injected stitch rollback interruption");
          }
        }
        error.clear();
        status = fsync_stitch_path(root_directory.path(), true);
        if (!status.ok())
          return status;
        status = publish_transaction_state(transaction, "RESTORED\n");
        if (!status.ok())
          return status;
        if (const char* interrupt = std::getenv("HM_TEST_STITCH_INTERRUPT_AFTER_RESTORED_SYNC");
            interrupt != nullptr && std::string(interrupt) == "1") {
          return absl::InternalError("Injected stitch interruption after restored state");
        }
        status = remove_pinned_directory(transaction_directory, "previous", previous_directory);
        if (!status.ok())
          return status;
        status = fsync_stitch_path(transaction, true);
        if (!status.ok())
          return status;
        status = rebind_stitch_generation_artifact(transaction, root_directory.path());
        if (!status.ok())
          return status;
        status = mark_stitch_transaction_rolled_back(transaction);
        if (!status.ok())
          return status;
      }
    }
    auto cleanup = remove_pinned_directory(root_directory, directory_name, transaction_directory);
    if (!cleanup.ok())
      return cleanup;
  }
  if (recovered) {
    auto status = fsync_stitch_path(root_directory.path(), true);
    if (!status.ok())
      return status;
  }
  return complete_transaction_recovery(root, TransactionJournalKind::kStitch);
}

CanvasConstraintArtifactLock::~CanvasConstraintArtifactLock() {
  if (descriptor_ >= 0) {
    ::flock(descriptor_, LOCK_UN);
    ::close(descriptor_);
  }
}

std::optional<size_t> live_stitch_max_canvas_dimension() {
  return live_canvas_limit_impl();
}

absl::StatusOr<LightweightCanvasConstraintCheck> try_lock_canvas_constraint_check(
    const fs::path& game_dir,
    size_t max_output_width) {
  auto lock = try_lock_canvas_constraint_artifacts(game_dir);
  if (!lock.ok())
    return lock.status();
  if (!*lock) {
    return LightweightCanvasConstraintCheck{
        .artifact_lock = nullptr,
        .artifacts_compatible = false,
        .requires_regeneration = true,
    };
  }
  auto compatibility = check_canvas_constraint_locked(game_dir, max_output_width);
  if (!compatibility.ok())
    return compatibility.status();
  return LightweightCanvasConstraintCheck{
      .artifact_lock = std::move(*lock),
      .artifacts_compatible = compatibility->artifacts_compatible,
      .requires_regeneration = compatibility->requires_regeneration,
  };
}

absl::StatusOr<std::unique_ptr<CanvasConstraintArtifactLock>> try_lock_canvas_constraint_artifacts(
    const fs::path& game_dir) {
  auto descriptor = lock_canvas_constraint_artifacts_impl(game_dir, /*wait=*/false);
  if (!descriptor.ok())
    return descriptor.status();
  return *descriptor < 0 ? std::unique_ptr<CanvasConstraintArtifactLock>()
                         : std::unique_ptr<CanvasConstraintArtifactLock>(new CanvasConstraintArtifactLock(*descriptor));
}

absl::StatusOr<std::unique_ptr<CanvasConstraintArtifactLock>> lock_canvas_constraint_artifacts(
    const fs::path& game_dir) {
  auto descriptor = lock_canvas_constraint_artifacts_impl(game_dir, /*wait=*/true);
  if (!descriptor.ok())
    return descriptor.status();
  return std::unique_ptr<CanvasConstraintArtifactLock>(new CanvasConstraintArtifactLock(*descriptor));
}

absl::StatusOr<std::string> stitch_artifact_generation_id_locked(const fs::path& game_dir) {
  const bool unreliable_filesystem = stitch_filesystem_has_unreliable_metadata(game_dir);
  auto identity = read_stitch_generation_artifact(game_dir / kStitchGenerationArtifact);
  if (!identity.ok()) {
    if (absl::IsNotFound(identity.status())) {
      auto bindings = stitch_artifact_stat_id(game_dir, StitchStatIdentityFormat::kBinding);
      if (!bindings.ok())
        return bindings.status();
      auto fingerprint = stitch_artifact_fingerprint(game_dir);
      if (!fingerprint.ok())
        return fingerprint.status();
      auto verified_bindings = stitch_artifact_stat_id(game_dir, StitchStatIdentityFormat::kBinding);
      if (!verified_bindings.ok())
        return verified_bindings.status();
      if (*verified_bindings != *bindings)
        return absl::AbortedError("Hugin artifacts changed while adopting their generation identity");
      const std::string logical_id = content_scoped_stitch_generation_id(*fingerprint);
      auto status = publish_stitch_file_atomically(
          game_dir / kStitchGenerationArtifact,
          serialize_stitch_generation_identity(logical_id, *bindings, *fingerprint));
      if (!status.ok())
        return status;
      return logical_id;
    }
    return identity.status();
  }
  auto bindings = stitch_artifact_stat_id(game_dir, StitchStatIdentityFormat::kBinding);
  if (!bindings.ok())
    return bindings.status();
  std::string logical_id;
  std::string recorded_fingerprint;
  bool bindings_match = false;
  if (identity->rfind("version=1\n", 0) == 0) {
    auto metadata = stitch_artifact_stat_id(game_dir, StitchStatIdentityFormat::kPortableMetadata);
    if (!metadata.ok())
      return metadata.status();
    auto legacy_logical_id = logical_stitch_generation_id_v1(*identity, *metadata);
    if (!legacy_logical_id.ok())
      return legacy_logical_id.status();
    logical_id = std::move(*legacy_logical_id);
  } else {
    auto parsed = parse_stitch_generation_identity(*identity);
    if (!parsed.ok())
      return parsed.status();
    bindings_match = parsed->bindings == *bindings;
    if (parsed->version == 3 && bindings_match && !unreliable_filesystem)
      return parsed->logical_id;
    logical_id = parsed->version == 2 && parsed->bindings != *bindings
        ? legacy_v2_mismatched_stitch_generation_id(parsed->logical_id, *bindings)
        : parsed->logical_id;
    recorded_fingerprint = std::move(parsed->fingerprint);
  }

  auto fingerprint = stitch_artifact_fingerprint(game_dir);
  if (!fingerprint.ok())
    return fingerprint.status();
  auto verified_bindings = stitch_artifact_stat_id(game_dir, StitchStatIdentityFormat::kBinding);
  if (!verified_bindings.ok())
    return verified_bindings.status();
  if (*verified_bindings != *bindings)
    return absl::AbortedError("Hugin artifacts changed while resolving their generation identity");
  if (!recorded_fingerprint.empty() && recorded_fingerprint != *fingerprint) {
    auto replacement_logical_id = random_stitch_logical_generation_id();
    if (!replacement_logical_id.ok())
      return replacement_logical_id.status();
    logical_id = std::move(*replacement_logical_id);
  }
  if (bindings_match && recorded_fingerprint == *fingerprint)
    return logical_id;
  auto status = publish_stitch_file_atomically(
      game_dir / kStitchGenerationArtifact, serialize_stitch_generation_identity(logical_id, *bindings, *fingerprint));
  if (!status.ok())
    return status;
  return logical_id;
}

absl::StatusOr<StitchArtifactContentIdentity> stitch_artifact_content_identity_locked(
    const fs::path& artifact_directory) {
  auto fingerprint = stitch_artifact_fingerprint(artifact_directory);
  if (!fingerprint.ok())
    return fingerprint.status();
  auto identity = read_stitch_generation_artifact(artifact_directory / kStitchGenerationArtifact);
  if (!identity.ok()) {
    if (absl::IsNotFound(identity.status())) {
      return StitchArtifactContentIdentity{
          .generation_id = content_scoped_stitch_generation_id(*fingerprint),
          .fingerprint = std::move(*fingerprint),
      };
    }
    return identity.status();
  }
  auto parsed = parse_stitch_generation_identity(*identity);
  if (!parsed.ok() || parsed->version != 3 || parsed->fingerprint != *fingerprint) {
    return absl::FailedPreconditionError(
        "Hugin artifact contents do not have a matching version-3 generation identity");
  }
  return StitchArtifactContentIdentity{
      .generation_id = std::move(parsed->logical_id),
      .fingerprint = std::move(*fingerprint),
  };
}

absl::StatusOr<std::string> stitch_artifact_preflight_generation_id_locked(const fs::path& game_dir) {
  auto identity = read_stitch_generation_artifact(game_dir / kStitchGenerationArtifact);
  if (!identity.ok()) {
    if (absl::IsNotFound(identity.status()))
      return stitch_artifact_stat_id(game_dir, StitchStatIdentityFormat::kLegacyGeneration);
    return stitch_artifact_generation_id_locked(game_dir);
  }
  auto parsed = parse_stitch_generation_identity(*identity);
  if (!parsed.ok() || parsed->version != 3)
    return stitch_artifact_generation_id_locked(game_dir);
  auto bindings = stitch_artifact_stat_id(game_dir, StitchStatIdentityFormat::kBinding);
  if (!bindings.ok())
    return bindings.status();
  if (parsed->bindings != *bindings)
    return stitch_artifact_generation_id_locked(game_dir);
  return parsed->logical_id;
}

absl::StatusOr<std::string> stitch_artifact_binding_revision_locked(const fs::path& game_dir) {
  return stitch_artifact_stat_id(game_dir, StitchStatIdentityFormat::kBinding);
}

absl::StatusOr<std::string> stitch_artifact_revision_locked(const fs::path& game_dir) {
  std::string identity;
  auto current_identity = read_stitch_generation_artifact(game_dir / kStitchGenerationArtifact);
  if (current_identity.ok()) {
    identity = std::move(*current_identity);
  } else if (!absl::IsNotFound(current_identity.status())) {
    return current_identity.status();
  }
  auto bindings = stitch_artifact_stat_id(game_dir, StitchStatIdentityFormat::kBinding);
  if (!bindings.ok())
    return bindings.status();
  std::ostringstream revision;
  revision << "hstream-stitch-revision-v1\nidentity-bytes=" << identity.size() << '\n'
           << identity << "bindings-bytes=" << bindings->size() << '\n'
           << *bindings;
  return revision.str();
}

bool stitch_artifact_metadata_is_reliable(const fs::path& game_dir) {
  return !stitch_filesystem_has_unreliable_metadata(game_dir);
}

absl::StatusOr<CanvasConstraintCompatibility> check_canvas_constraint_locked_impl(
    const fs::path& game_dir,
    size_t max_output_width,
    bool validate_seam_payload) {
  const bool has_mappings = any_mapping_artifact_exists(game_dir);
  const absl::Status dependency_status = dependency_tree_status(game_dir);
  if (!dependency_status.ok()) {
    if (absl::IsNotFound(dependency_status) || absl::IsFailedPrecondition(dependency_status))
      return CanvasConstraintCompatibility{.requires_regeneration = has_mappings};
    return dependency_status;
  }
  const absl::Status artifact_bounds = validate_stitch_generation_artifact_bounds_locked(game_dir);
  if (!artifact_bounds.ok()) {
    if (absl::IsNotFound(artifact_bounds) || absl::IsFailedPrecondition(artifact_bounds) ||
        absl::IsInvalidArgument(artifact_bounds) || absl::IsResourceExhausted(artifact_bounds)) {
      return CanvasConstraintCompatibility{.requires_regeneration = has_mappings};
    }
    return artifact_bounds;
  }
  auto canvas = mapping_canvas_size(game_dir);
  if (!canvas.ok()) {
    if (absl::IsNotFound(canvas.status()) || absl::IsFailedPrecondition(canvas.status()) ||
        absl::IsInvalidArgument(canvas.status()) || absl::IsResourceExhausted(canvas.status())) {
      return CanvasConstraintCompatibility{.requires_regeneration = has_mappings};
    }
    return canvas.status();
  }
  auto provenance = read_canvas_provenance(game_dir);
  if (!provenance.ok()) {
    if (absl::IsNotFound(provenance.status()) || absl::IsFailedPrecondition(provenance.status()) ||
        absl::IsInvalidArgument(provenance.status())) {
      return CanvasConstraintCompatibility{.requires_regeneration = has_mappings};
    }
    return provenance.status();
  }
  const bool compatible =
      artifacts_are_compatible(*provenance, *canvas, max_output_width, live_stitch_max_canvas_dimension().value_or(0));
  if (!compatible) {
    return CanvasConstraintCompatibility{
        .artifacts_compatible = false,
        .requires_regeneration = true,
    };
  }
  const absl::Status artifact_contract = validate_canvas_artifact_contract(game_dir, *canvas, validate_seam_payload);
  if (!artifact_contract.ok()) {
    if (absl::IsNotFound(artifact_contract) || absl::IsFailedPrecondition(artifact_contract) ||
        absl::IsInvalidArgument(artifact_contract) || absl::IsResourceExhausted(artifact_contract)) {
      return CanvasConstraintCompatibility{.requires_regeneration = has_mappings};
    }
    return artifact_contract;
  }
  return CanvasConstraintCompatibility{
      .artifacts_compatible = true,
      .requires_regeneration = false,
  };
}

absl::StatusOr<CanvasConstraintCompatibility> check_canvas_constraint_locked(
    const fs::path& game_dir,
    size_t max_output_width) {
  return check_canvas_constraint_locked_impl(game_dir, max_output_width, /*validate_seam_payload=*/true);
}

absl::StatusOr<CanvasConstraintCompatibility> check_canvas_constraint_metadata_locked(
    const fs::path& game_dir,
    size_t max_output_width) {
  return check_canvas_constraint_locked_impl(game_dir, max_output_width, /*validate_seam_payload=*/false);
}

} // namespace hm::stitching
