#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include <fcntl.h>
#include <png.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <tiffio.h>
#include <unistd.h>

#include "absl/status/status.h"

namespace hm::stitching {
namespace {

namespace fs = std::filesystem;

constexpr size_t kDefaultJetsonMaxLiveStitchCanvasDimension = 8192;
constexpr size_t kHardMaximumArtifactDimension = 32768;
constexpr uint64_t kHardMaximumArtifactPixels = 128ULL * 1024ULL * 1024ULL;
constexpr const char* kStitchTransactionPrefix = ".hstream-stitch-";

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

std::optional<size_t> live_canvas_limit() {
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

absl::Status validate_canvas_artifact_contract(const fs::path& game_dir, const CanvasSize& canvas) {
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
  return validate_nonuniform_png(seam_path, *layout);
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

absl::StatusOr<CanvasProvenance> read_canvas_provenance(const fs::path& game_dir) {
  std::ifstream input(game_dir / "stitching_canvas_provenance");
  if (!input)
    return absl::NotFoundError("Canvas provenance is missing");
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

absl::StatusOr<std::string> read_stitch_transaction_state(const fs::path& transaction) {
  const fs::path state_path = transaction / "state";
  std::error_code error;
  const bool exists = fs::exists(state_path, error);
  if (error)
    return absl::InternalError("Unable to inspect stitch transaction state: " + error.message());
  if (!exists)
    return std::string("UNPREPARED");
  const int descriptor = ::open(state_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
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
  if (contents == "COMMITTED\n")
    return std::string("COMMITTED");
  if (contents == "ROLLED_BACK\n")
    return std::string("ROLLED_BACK");
  return absl::FailedPreconditionError("Invalid durable stitch transaction state contents");
}

} // namespace

const std::vector<std::string>& required_stitch_artifact_names() {
  return kRequiredArtifacts;
}

const std::vector<std::string>& stitch_artifact_names() {
  return kArtifacts;
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

absl::Status link_stitch_file_preserving_identity(const fs::path& source, const fs::path& destination) {
  if (::link(source.c_str(), destination.c_str()) != 0) {
    return absl::InternalError(
        "Unable to hard-link stitch artifact for rollback: " + std::string(std::strerror(errno)));
  }
  return fsync_stitch_path(destination);
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

absl::Status mark_stitch_transaction_rolled_back(const fs::path& transaction) {
  const fs::path temporary = transaction / "state.rolled_back";
  auto status = write_stitch_transaction_file(temporary, "ROLLED_BACK\n");
  if (!status.ok())
    return status;
  std::error_code error;
  fs::rename(temporary, transaction / "state", error);
  if (error)
    return absl::InternalError("Unable to commit Hugin rollback state: " + error.message());
  return fsync_stitch_path(transaction, true);
}

absl::Status recover_stitch_transactions_locked(const fs::path& root) {
  std::error_code error;
  bool recovered = false;
  for (const auto& entry : fs::directory_iterator(root, error)) {
    if (error)
      return absl::InternalError("Unable to inspect stitch transactions: " + error.message());
    const std::string directory_name = entry.path().filename().string();
    if (!entry.is_directory(error) || error || directory_name.rfind(kStitchTransactionPrefix, 0) != 0) {
      error.clear();
      continue;
    }
    recovered = true;
    const fs::path transaction = entry.path();
    auto state = read_stitch_transaction_state(transaction);
    if (!state.ok())
      return state.status();
    if (*state == "PREPARED") {
      std::ifstream manifest(transaction / "artifacts");
      if (!manifest)
        return absl::FailedPreconditionError("Prepared stitch transaction has no artifact manifest");
      std::set<std::string> manifested;
      for (std::string name; manifest >> name;) {
        if (fs::path(name).filename() != name || !is_stitch_artifact_name(name) || !manifested.insert(name).second)
          return absl::InvalidArgumentError("Invalid stitch transaction filename: " + name);
      }
      const std::set<std::string> current(kArtifacts.begin(), kArtifacts.end());
      if (!manifest.eof() ||
          (manifested != current && manifested != kPreviousArtifacts && manifested != kLegacyArtifacts)) {
        return absl::FailedPreconditionError("Prepared stitch transaction has an incomplete artifact manifest");
      }
      const fs::path previous = transaction / "previous";
      std::vector<fs::path> backups;
      const bool previous_exists = fs::exists(previous, error);
      if (error)
        return absl::InternalError("Unable to inspect stitch transaction backup directory: " + error.message());
      if (previous_exists) {
        if (!fs::is_directory(previous, error) || error)
          return absl::FailedPreconditionError("Stitch transaction backup is not a directory");
        for (const auto& old : fs::directory_iterator(previous, error)) {
          if (error)
            return absl::InternalError("Unable to inspect stitch transaction backup: " + error.message());
          const std::string old_name = old.path().filename().string();
          if (!old.is_regular_file(error) || error || !is_stitch_artifact_name(old_name))
            return absl::InvalidArgumentError("Invalid stitch transaction backup: " + old_name);
          backups.push_back(old.path());
        }
      }
      for (const std::string& name : manifested) {
        fs::remove(root / name, error);
        if (error)
          return absl::InternalError("Unable to remove interrupted stitch artifact: " + error.message());
      }
      for (const fs::path& old : backups) {
        auto status = link_stitch_file_preserving_identity(old, root / old.filename());
        if (!status.ok())
          return absl::InternalError("Unable to restore interrupted stitch artifact: " + std::string(status.message()));
      }
      error.clear();
      auto status = fsync_stitch_path(root, true);
      if (!status.ok())
        return status;
      status = mark_stitch_transaction_rolled_back(transaction);
      if (!status.ok())
        return status;
    }
    fs::remove_all(transaction, error);
    if (error)
      return absl::InternalError("Unable to clean stitch transaction: " + error.message());
  }
  return recovered ? fsync_stitch_path(root, true) : absl::OkStatus();
}

CanvasConstraintArtifactLock::~CanvasConstraintArtifactLock() {
  if (descriptor_ >= 0) {
    ::flock(descriptor_, LOCK_UN);
    ::close(descriptor_);
  }
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
  std::error_code error;
  if (!fs::is_directory(game_dir, error) || error)
    return absl::NotFoundError("Canvas compatibility check requires an existing game directory");
  const fs::path lock_path = game_dir / ".hstream-stitch.lock";
  const int descriptor = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (descriptor < 0)
    return absl::InternalError("Unable to open stitching artifact lock: " + std::string(std::strerror(errno)));
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    const int lock_error = errno;
    ::close(descriptor);
    if (lock_error == EWOULDBLOCK || lock_error == EAGAIN) {
      return std::unique_ptr<CanvasConstraintArtifactLock>();
    }
    return absl::InternalError("Unable to lock stitching artifacts: " + std::string(std::strerror(lock_error)));
  }
  auto recovery = recover_stitch_transactions_locked(game_dir);
  if (!recovery.ok()) {
    ::flock(descriptor, LOCK_UN);
    ::close(descriptor);
    return recovery;
  }
  auto lock = std::unique_ptr<CanvasConstraintArtifactLock>(new CanvasConstraintArtifactLock(descriptor));
  return lock;
}

absl::StatusOr<CanvasConstraintCompatibility> check_canvas_constraint_locked(
    const fs::path& game_dir,
    size_t max_output_width) {
  const bool has_mappings = any_mapping_artifact_exists(game_dir);
  const absl::Status dependency_status = dependency_tree_status(game_dir);
  if (!dependency_status.ok()) {
    if (absl::IsNotFound(dependency_status) || absl::IsFailedPrecondition(dependency_status))
      return CanvasConstraintCompatibility{.requires_regeneration = has_mappings};
    return dependency_status;
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
      artifacts_are_compatible(*provenance, *canvas, max_output_width, live_canvas_limit().value_or(0));
  if (!compatible) {
    return CanvasConstraintCompatibility{
        .artifacts_compatible = false,
        .requires_regeneration = true,
    };
  }
  const absl::Status artifact_contract = validate_canvas_artifact_contract(game_dir, *canvas);
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

} // namespace hm::stitching
