#include "hstream/src/libs/stitching/HuginProject.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <sys/file.h>
#include <sys/stat.h>
#include <tiffio.h>
#include <unistd.h>

#include "absl/status/status.h"
#include "hstream/src/libs/common/Process.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/HomographyMaps.h"
#include "hstream/src/libs/stitching/TransactionState.h"

extern "C" char** environ;

namespace hm::stitching {

bool hard_seam_fallback_enabled() {
  const char* value = std::getenv("HM_ALLOW_HARD_SEAM_FALLBACK");
  return value != nullptr && std::strcmp(value, "1") == 0;
}

namespace {

namespace fs = std::filesystem;

constexpr size_t kMinimumUsableMatches = 16;
constexpr double kMaximumOptimizationRmsPixels = 50.0;
constexpr size_t kHardMaximumCanvasDimension = 32768;
constexpr uint64_t kHardMaximumCanvasPixels = 128ULL * 1024ULL * 1024ULL;
absl::StatusOr<std::string> read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return absl::NotFoundError("Unable to read Hugin file: " + path.string());
  std::ostringstream contents;
  contents << input.rdbuf();
  if (!input.good() && !input.eof())
    return absl::InternalError("Failed reading Hugin file: " + path.string());
  return contents.str();
}

absl::Status write_file(const fs::path& path, const std::string& contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    return absl::InternalError("Unable to write Hugin file: " + path.string());
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.flush();
  if (!output)
    return absl::InternalError("Failed writing Hugin file: " + path.string());
  return absl::OkStatus();
}

absl::StatusOr<size_t> parse_canvas_provenance_value(const std::string& line, const std::string& key) {
  const std::string prefix = key + "=";
  if (line.rfind(prefix, 0) != 0 || line.size() == prefix.size())
    return absl::FailedPreconditionError("Invalid stitching canvas provenance field: " + key);
  size_t value = 0;
  const char* begin = line.data() + prefix.size();
  const char* end = line.data() + line.size();
  const auto parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc() || parsed.ptr != end)
    return absl::FailedPreconditionError("Invalid stitching canvas provenance value: " + key);
  return value;
}

absl::StatusOr<HuginProject::CanvasProvenance> parse_canvas_provenance(const std::string& contents) {
  std::istringstream input(contents);
  std::vector<std::string> lines;
  for (std::string line; std::getline(input, line);)
    lines.push_back(std::move(line));
  if (lines.size() != 9 || lines[0] != "version=2")
    return absl::FailedPreconditionError("Invalid stitching canvas provenance format");
  HuginProject::CanvasProvenance provenance;
  HM_ASSIGN_OR_RETURN(provenance.max_output_width, parse_canvas_provenance_value(lines[1], "max-output-width"));
  HM_ASSIGN_OR_RETURN(provenance.max_canvas_dimension, parse_canvas_provenance_value(lines[2], "max-canvas-dimension"));
  HM_ASSIGN_OR_RETURN(provenance.source_canvas_width, parse_canvas_provenance_value(lines[3], "source-canvas-width"));
  HM_ASSIGN_OR_RETURN(provenance.source_canvas_height, parse_canvas_provenance_value(lines[4], "source-canvas-height"));
  HM_ASSIGN_OR_RETURN(provenance.canvas_width, parse_canvas_provenance_value(lines[5], "canvas-width"));
  HM_ASSIGN_OR_RETURN(provenance.canvas_height, parse_canvas_provenance_value(lines[6], "canvas-height"));
  size_t max_output_width_applied = 0;
  size_t max_canvas_dimension_applied = 0;
  HM_ASSIGN_OR_RETURN(max_output_width_applied, parse_canvas_provenance_value(lines[7], "max-output-width-applied"));
  HM_ASSIGN_OR_RETURN(
      max_canvas_dimension_applied, parse_canvas_provenance_value(lines[8], "max-canvas-dimension-applied"));
  if (provenance.source_canvas_width == 0 || provenance.source_canvas_height == 0 || provenance.canvas_width == 0 ||
      provenance.canvas_height == 0 || provenance.source_canvas_width < provenance.canvas_width ||
      provenance.source_canvas_height < provenance.canvas_height) {
    return absl::FailedPreconditionError("Stitching canvas provenance dimensions must be positive");
  }
  if (max_output_width_applied > 1 || max_canvas_dimension_applied > 1 ||
      (max_output_width_applied != 0 && provenance.max_output_width == 0) ||
      (max_canvas_dimension_applied != 0 && provenance.max_canvas_dimension == 0)) {
    return absl::FailedPreconditionError("Invalid stitching canvas provenance constraint state");
  }
  provenance.max_output_width_applied = max_output_width_applied != 0;
  provenance.max_canvas_dimension_applied = max_canvas_dimension_applied != 0;
  return provenance;
}

std::map<std::string, std::string> environment() {
  std::map<std::string, std::string> values;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string item(*entry);
    const size_t separator = item.find('=');
    if (separator != std::string::npos)
      values[item.substr(0, separator)] = item.substr(separator + 1);
  }
  values["LC_ALL"] = "C";
  return values;
}

absl::StatusOr<std::string> executable(const char* override_name, const char* name) {
  if (const char* override_path = std::getenv(override_name); override_path != nullptr && *override_path != '\0') {
    if (::access(override_path, X_OK) == 0)
      return std::string(override_path);
    return absl::NotFoundError(std::string(override_name) + " is not executable: " + override_path);
  }
  const fs::path system_path = fs::path("/usr/bin") / name;
  if (::access(system_path.c_str(), X_OK) == 0)
    return system_path.string();
  auto found = hm::findExecutable(name, {"PATH"});
  if (found.has_value() && ::access(found->c_str(), X_OK) == 0)
    return *found;
  return absl::NotFoundError(std::string("Required Hugin executable not found: ") + name);
}

absl::Status run_checked(
    const std::vector<std::string>& command,
    const fs::path& working_dir,
    std::string* captured = nullptr,
    const std::function<bool()>& is_cancelled = {}) {
  const int exit_code = hm::run_command(
      command,
      working_dir.string(),
      environment(),
      [captured](const std::string& error, const std::string& output) {
        if (captured != nullptr) {
          captured->append(error);
          captured->push_back('\n');
          captured->append(output);
          captured->push_back('\n');
        }
        if (!error.empty())
          std::cerr << error << '\n';
        if (!output.empty())
          std::cout << output << '\n';
      },
      is_cancelled);
  if (is_cancelled && is_cancelled()) {
    return absl::CancelledError("Hugin calibration command cancelled");
  }
  if (exit_code != 0) {
    std::ostringstream message;
    message << "Command failed with exit code " << exit_code << ':';
    for (const std::string& argument : command)
      message << ' ' << argument;
    return absl::InternalError(message.str());
  }
  return absl::OkStatus();
}

absl::Status validate_nonempty_file(const fs::path& path) {
  std::error_code error;
  if (!fs::is_regular_file(path, error) || error) {
    return absl::NotFoundError("Expected Hugin artifact is missing: " + path.string());
  }
  if (fs::file_size(path, error) == 0 || error) {
    return absl::FailedPreconditionError("Expected Hugin artifact is empty: " + path.string());
  }
  return absl::OkStatus();
}

absl::StatusOr<int> lock_stitch_transactions(const fs::path& root) {
  const fs::path path = root / ".hstream-stitch.lock";
  const int descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (descriptor < 0)
    return absl::InternalError("Unable to open stitch transaction lock: " + std::string(std::strerror(errno)));
  if (::flock(descriptor, LOCK_EX) != 0) {
    const std::string message = std::strerror(errno);
    ::close(descriptor);
    return absl::InternalError("Unable to lock stitch transaction: " + message);
  }
  return descriptor;
}

struct TiffPlacement {
  float x{0.0f};
  float y{0.0f};
  int width{0};
  int height{0};
};

absl::Status validate_decoded_dimensions(
    uint64_t width,
    uint64_t height,
    const std::optional<size_t>& maximum_dimension,
    const std::string& description) {
  if (width == 0 || height == 0 || width > kHardMaximumCanvasDimension || height > kHardMaximumCanvasDimension ||
      width > kHardMaximumCanvasPixels / height) {
    return absl::ResourceExhaustedError(description + " exceeds the absolute decoded-image safety limit");
  }
  if (maximum_dimension.has_value() && (width > *maximum_dimension || height > *maximum_dimension)) {
    return absl::FailedPreconditionError(description + " exceeds the configured maximum canvas dimension");
  }
  return absl::OkStatus();
}

absl::StatusOr<TiffPlacement> read_tiff_placement(
    const fs::path& path,
    const std::optional<size_t>& maximum_dimension) {
  TIFF* tif = TIFFOpen(path.c_str(), "r");
  if (tif == nullptr)
    return absl::InvalidArgumentError("Unable to decode Hugin TIFF: " + path.string());
  uint32_t width = 0;
  uint32_t height = 0;
  float x_resolution = 0.0f;
  float y_resolution = 0.0f;
  float x_position = 0.0f;
  float y_position = 0.0f;
  const bool metadata_valid = TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width) &&
      TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height) && TIFFGetField(tif, TIFFTAG_XRESOLUTION, &x_resolution) &&
      TIFFGetField(tif, TIFFTAG_YRESOLUTION, &y_resolution);
  const bool placement_valid =
      TIFFGetField(tif, TIFFTAG_XPOSITION, &x_position) && TIFFGetField(tif, TIFFTAG_YPOSITION, &y_position);
  auto dimensions_status = validate_decoded_dimensions(width, height, maximum_dimension, "Hugin TIFF " + path.string());
  const tmsize_t scanline_size = dimensions_status.ok() ? TIFFScanlineSize(tif) : 0;
  bool pixels_valid = dimensions_status.ok() && scanline_size > 0 && scanline_size <= 256 * 1024 * 1024;
  if (pixels_valid) {
    std::vector<unsigned char> scanline(static_cast<size_t>(scanline_size));
    pixels_valid = TIFFReadScanline(tif, scanline.data(), 0, 0) >= 0;
    if (pixels_valid && height > 1)
      pixels_valid = TIFFReadScanline(tif, scanline.data(), height - 1, 0) >= 0;
  }
  TIFFClose(tif);
  if (!dimensions_status.ok())
    return dimensions_status;
  if (!metadata_valid || !placement_valid || !pixels_valid || width == 0 || height == 0 ||
      width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max() ||
      !std::isfinite(x_resolution) || !std::isfinite(y_resolution) || x_resolution <= 0.0f || y_resolution <= 0.0f ||
      !std::isfinite(x_position) || !std::isfinite(y_position)) {
    return absl::FailedPreconditionError("Hugin TIFF metadata or pixel data is invalid: " + path.string());
  }
  const float x = x_position * x_resolution;
  const float y = y_position * y_resolution;
  if (!std::isfinite(x) || !std::isfinite(y))
    return absl::FailedPreconditionError("Hugin TIFF placement overflows pixel coordinates: " + path.string());
  return TiffPlacement{x, y, static_cast<int>(width), static_cast<int>(height)};
}

absl::Status inspect_remap_tiff(
    const fs::path& path,
    const TiffPlacement& placement,
    const std::optional<size_t>& maximum_dimension) {
  TIFF* tif = TIFFOpen(path.c_str(), "r");
  if (tif == nullptr)
    return absl::InvalidArgumentError("Unable to inspect Hugin remap TIFF: " + path.string());
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t samples = 0;
  uint16_t bits = 0;
  uint16_t sample_format = SAMPLEFORMAT_UINT;
  uint16_t planar = PLANARCONFIG_CONTIG;
  uint16_t orientation = ORIENTATION_TOPLEFT;
  const bool metadata_valid =
      TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width) && TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
  TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sample_format);
  TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
  TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, &orientation);
  TIFFClose(tif);
  if (!metadata_valid || samples != 1 || bits != 16 || sample_format != SAMPLEFORMAT_UINT ||
      planar != PLANARCONFIG_CONTIG || orientation != ORIENTATION_TOPLEFT ||
      width != static_cast<uint32_t>(placement.width) || height != static_cast<uint32_t>(placement.height)) {
    return absl::FailedPreconditionError("Hugin remap TIFF header violates the CV_16U contract: " + path.string());
  }
  return validate_decoded_dimensions(width, height, maximum_dimension, "Hugin remap TIFF " + path.string());
}

struct PngLayout {
  int width{0};
  int height{0};
  int offset_x{0};
  int offset_y{0};
  bool has_offset{false};
};

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
  if (!input || file_signature != signature) {
    return absl::FailedPreconditionError("Invalid PNG header: " + path.string());
  }
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
      if (width == 0 || height == 0 || width > std::numeric_limits<int>::max() ||
          height > std::numeric_limits<int>::max()) {
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
  if (!have_header)
    return absl::FailedPreconditionError("PNG is missing its IHDR chunk: " + path.string());
  if (!have_end)
    return absl::FailedPreconditionError("PNG is missing its IEND chunk: " + path.string());
  return layout;
}

absl::StatusOr<std::pair<int, int>> read_png_dimensions(const fs::path& path) {
  auto layout = read_png_layout(path);
  if (!layout.ok())
    return layout.status();
  return std::make_pair(layout->width, layout->height);
}

absl::StatusOr<cv::Mat> decode_nonuniform_seam(const fs::path& path, const PngLayout& layout) {
  cv::Mat seam;
  try {
    seam = cv::imread(path.string(), cv::IMREAD_GRAYSCALE);
  } catch (const cv::Exception& exception) {
    return absl::ResourceExhaustedError("Unable to safely decode seam: " + std::string(exception.what()));
  } catch (const std::bad_alloc&) {
    return absl::ResourceExhaustedError("Unable to allocate decoded seam");
  }
  if (seam.empty() || seam.type() != CV_8UC1 || seam.cols != layout.width || seam.rows != layout.height)
    return absl::FailedPreconditionError("PNG seam is not a decodable 8-bit grayscale image: " + path.string());
  double minimum = 0.0;
  double maximum = 0.0;
  cv::minMaxLoc(seam, &minimum, &maximum);
  if (maximum <= minimum)
    return absl::FailedPreconditionError("PNG seam is uniform: " + path.string());
  return seam;
}

absl::Status publish_normalized_seam(const fs::path& path, const cv::Mat& seam, int canvas_width, int canvas_height) {
  std::vector<unsigned char> encoded;
  if (!cv::imencode(".png", seam, encoded) || encoded.empty())
    return absl::InternalError("Unable to encode normalized seam: " + path.string());

  const fs::path parent = path.parent_path().empty() ? fs::path(".") : path.parent_path();
  std::string pattern = (parent / ("." + path.filename().string() + ".normalize-XXXXXX.png")).string();
  std::vector<char> writable_pattern(pattern.begin(), pattern.end());
  writable_pattern.push_back('\0');
  const int descriptor = ::mkstemps(writable_pattern.data(), 4);
  if (descriptor < 0)
    return absl::InternalError("Unable to create normalized seam temporary file: " + std::string(std::strerror(errno)));
  const fs::path temporary(writable_pattern.data());
  struct TemporaryCleanup {
    int descriptor;
    fs::path path;
    ~TemporaryCleanup() {
      if (descriptor >= 0)
        ::close(descriptor);
      if (!path.empty()) {
        std::error_code ignored;
        fs::remove(path, ignored);
      }
    }
  } cleanup{descriptor, temporary};

  struct stat source_metadata{};
  if (::stat(path.c_str(), &source_metadata) != 0)
    return absl::InternalError("Unable to read normalized seam source mode: " + std::string(std::strerror(errno)));
  if (!S_ISREG(source_metadata.st_mode))
    return absl::FailedPreconditionError("Normalized seam source is not a regular file: " + path.string());
  if (::fchmod(descriptor, source_metadata.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) != 0)
    return absl::InternalError("Unable to preserve normalized seam mode: " + std::string(std::strerror(errno)));

  size_t written = 0;
  while (written < encoded.size()) {
    const ssize_t count = ::write(descriptor, encoded.data() + written, encoded.size() - written);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return absl::InternalError(
          "Unable to write normalized seam temporary file: " + std::string(std::strerror(errno)));
    written += static_cast<size_t>(count);
  }
  if (::fsync(descriptor) != 0)
    return absl::InternalError("Unable to fsync normalized seam temporary file: " + std::string(std::strerror(errno)));
  if (::close(descriptor) != 0) {
    cleanup.descriptor = -1;
    return absl::InternalError("Unable to close normalized seam temporary file: " + std::string(std::strerror(errno)));
  }
  cleanup.descriptor = -1;

  auto temporary_layout = read_png_layout(temporary);
  if (!temporary_layout.ok())
    return temporary_layout.status();
  if (temporary_layout->offset_x != 0 || temporary_layout->offset_y != 0 || temporary_layout->width != canvas_width ||
      temporary_layout->height != canvas_height) {
    return absl::FailedPreconditionError("Encoded normalized seam has an invalid layout: " + temporary.string());
  }
  auto decoded = decode_nonuniform_seam(temporary, *temporary_layout);
  if (!decoded.ok())
    return decoded.status();

  if (const char* interrupt = std::getenv("HM_TEST_SEAM_NORMALIZATION_FAIL_BEFORE_RENAME");
      interrupt != nullptr && std::strcmp(interrupt, "1") == 0) {
    return absl::InternalError("Injected seam normalization failure before atomic rename");
  }
  std::error_code error;
  fs::rename(temporary, path, error);
  if (error)
    return absl::InternalError("Unable to atomically publish normalized seam: " + error.message());
  cleanup.path.clear();
  return fsync_stitch_path(parent, true);
}

absl::Status validate_and_normalize_seam(const fs::path& path, int canvas_width, int canvas_height) {
  if (canvas_width <= 0 || canvas_height <= 0)
    return absl::InvalidArgumentError("Seam canvas dimensions must be positive");
  auto layout = read_png_layout(path);
  if (!layout.ok())
    return layout.status();
  const int64_t right = static_cast<int64_t>(layout->offset_x) + layout->width;
  const int64_t bottom = static_cast<int64_t>(layout->offset_y) + layout->height;
  if (layout->offset_x < 0 || layout->offset_y < 0 || right > canvas_width || bottom > canvas_height) {
    return absl::FailedPreconditionError(
        "PNG seam crop lies outside its mapping canvas: " + path.string() + " crop=" + std::to_string(layout->width) +
        "x" + std::to_string(layout->height) + "+" + std::to_string(layout->offset_x) + "+" +
        std::to_string(layout->offset_y) + " canvas=" + std::to_string(canvas_width) + "x" +
        std::to_string(canvas_height));
  }
  if (!layout->has_offset && layout->offset_x == 0 && layout->offset_y == 0 &&
      (layout->width != canvas_width || layout->height != canvas_height)) {
    return absl::FailedPreconditionError(
        "PNG seam has full-canvas origin but does not match its mapping canvas: " + path.string() +
        " size=" + std::to_string(layout->width) + "x" + std::to_string(layout->height) +
        " canvas=" + std::to_string(canvas_width) + "x" + std::to_string(canvas_height));
  }

  auto seam = decode_nonuniform_seam(path, *layout);
  if (!seam.ok())
    return seam.status();

  if (layout->offset_x != 0 || layout->offset_y != 0 || seam->cols != canvas_width || seam->rows != canvas_height) {
    cv::Mat normalized;
    try {
      cv::copyMakeBorder(
          *seam,
          normalized,
          layout->offset_y,
          canvas_height - static_cast<int>(bottom),
          layout->offset_x,
          canvas_width - static_cast<int>(right),
          cv::BORDER_REPLICATE);
      return publish_normalized_seam(path, normalized, canvas_width, canvas_height);
    } catch (const cv::Exception& exception) {
      return absl::ResourceExhaustedError("Unable to normalize seam: " + std::string(exception.what()));
    } catch (const std::bad_alloc&) {
      return absl::ResourceExhaustedError("Unable to allocate normalized seam");
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<PngLayout> validated_seam_layout(
    const fs::path& path,
    int native_canvas_width,
    int native_canvas_height,
    int effective_canvas_width,
    int effective_canvas_height) {
  if (native_canvas_width <= 0 || native_canvas_height <= 0 || effective_canvas_width <= 0 ||
      effective_canvas_height <= 0) {
    return absl::InvalidArgumentError("Seam canvas dimensions must be positive");
  }
  auto layout = read_png_layout(path);
  if (!layout.ok())
    return layout.status();
  const int64_t right = static_cast<int64_t>(layout->offset_x) + layout->width;
  const int64_t bottom = static_cast<int64_t>(layout->offset_y) + layout->height;
  const bool matches_native_canvas = layout->offset_x == 0 && layout->offset_y == 0 &&
      layout->width == native_canvas_width && layout->height == native_canvas_height;
  const bool matches_effective_canvas = layout->offset_x == 0 && layout->offset_y == 0 &&
      layout->width == effective_canvas_width && layout->height == effective_canvas_height;
  if (matches_native_canvas || matches_effective_canvas)
    return *layout;
  if (!layout->has_offset && layout->offset_x == 0 && layout->offset_y == 0) {
    return absl::FailedPreconditionError(
        "PNG seam has full-canvas origin but matches neither the native nor capped mapping canvas: " + path.string() +
        " size=" + std::to_string(layout->width) + "x" + std::to_string(layout->height) +
        " native-canvas=" + std::to_string(native_canvas_width) + "x" + std::to_string(native_canvas_height) +
        " capped-canvas=" + std::to_string(effective_canvas_width) + "x" + std::to_string(effective_canvas_height));
  }
  if (layout->offset_x < 0 || layout->offset_y < 0 || right > native_canvas_width || bottom > native_canvas_height) {
    return absl::FailedPreconditionError(
        "PNG seam crop lies outside its mapping canvas: " + path.string() + " crop=" + std::to_string(layout->width) +
        "x" + std::to_string(layout->height) + "+" + std::to_string(layout->offset_x) + "+" +
        std::to_string(layout->offset_y) + " canvas=" + std::to_string(native_canvas_width) + "x" +
        std::to_string(native_canvas_height));
  }
  return *layout;
}

absl::Status validate_seam_for_configured_artifacts(
    const fs::path& path,
    int native_canvas_width,
    int native_canvas_height,
    int effective_canvas_width,
    int effective_canvas_height) {
  auto layout = validated_seam_layout(
      path, native_canvas_width, native_canvas_height, effective_canvas_width, effective_canvas_height);
  if (!layout.ok())
    return layout.status();
  auto seam = decode_nonuniform_seam(path, *layout);
  return seam.ok() ? absl::OkStatus() : seam.status();
}

absl::Status validate_and_normalize_seam(
    const fs::path& path,
    int native_canvas_width,
    int native_canvas_height,
    int effective_canvas_width,
    int effective_canvas_height,
    double scale) {
  if (native_canvas_width <= 0 || native_canvas_height <= 0 || effective_canvas_width <= 0 ||
      effective_canvas_height <= 0) {
    return absl::InvalidArgumentError("Seam canvas dimensions must be positive");
  }
  if (!std::isfinite(scale) || scale <= 0.0) {
    return absl::InvalidArgumentError("Seam scale must be positive and finite");
  }
  if (native_canvas_width == effective_canvas_width && native_canvas_height == effective_canvas_height) {
    return validate_and_normalize_seam(path, native_canvas_width, native_canvas_height);
  }

  auto layout = read_png_layout(path);
  if (!layout.ok())
    return layout.status();
  const int64_t right = static_cast<int64_t>(layout->offset_x) + layout->width;
  const int64_t bottom = static_cast<int64_t>(layout->offset_y) + layout->height;
  const bool matches_native_canvas = layout->offset_x == 0 && layout->offset_y == 0 &&
      layout->width == native_canvas_width && layout->height == native_canvas_height;
  const bool matches_effective_canvas = layout->offset_x == 0 && layout->offset_y == 0 &&
      layout->width == effective_canvas_width && layout->height == effective_canvas_height;
  if (matches_effective_canvas) {
    auto seam = decode_nonuniform_seam(path, *layout);
    return seam.ok() ? absl::OkStatus() : seam.status();
  }
  if (!layout->has_offset && !matches_native_canvas) {
    return absl::FailedPreconditionError(
        "PNG seam has full-canvas origin but matches neither the native nor capped mapping canvas: " + path.string() +
        " size=" + std::to_string(layout->width) + "x" + std::to_string(layout->height) +
        " native-canvas=" + std::to_string(native_canvas_width) + "x" + std::to_string(native_canvas_height) +
        " capped-canvas=" + std::to_string(effective_canvas_width) + "x" + std::to_string(effective_canvas_height));
  }

  if (layout->offset_x < 0 || layout->offset_y < 0 || right > native_canvas_width || bottom > native_canvas_height) {
    return absl::FailedPreconditionError(
        "PNG seam crop lies outside its mapping canvas: " + path.string() + " crop=" + std::to_string(layout->width) +
        "x" + std::to_string(layout->height) + "+" + std::to_string(layout->offset_x) + "+" +
        std::to_string(layout->offset_y) + " canvas=" + std::to_string(native_canvas_width) + "x" +
        std::to_string(native_canvas_height));
  }

  auto seam = decode_nonuniform_seam(path, *layout);
  if (!seam.ok())
    return seam.status();

  const int scaled_left =
      std::clamp(static_cast<int>(std::floor(layout->offset_x * scale)), 0, effective_canvas_width - 1);
  const int scaled_top =
      std::clamp(static_cast<int>(std::floor(layout->offset_y * scale)), 0, effective_canvas_height - 1);
  const int scaled_right = std::clamp(
      static_cast<int>(std::ceil(static_cast<double>(right) * scale)), scaled_left + 1, effective_canvas_width);
  const int scaled_bottom = std::clamp(
      static_cast<int>(std::ceil(static_cast<double>(bottom) * scale)), scaled_top + 1, effective_canvas_height);

  cv::Mat scaled_crop;
  cv::Mat normalized;
  try {
    cv::resize(
        *seam,
        scaled_crop,
        cv::Size(scaled_right - scaled_left, scaled_bottom - scaled_top),
        0.0,
        0.0,
        cv::INTER_NEAREST);
    cv::copyMakeBorder(
        scaled_crop,
        normalized,
        scaled_top,
        effective_canvas_height - scaled_bottom,
        scaled_left,
        effective_canvas_width - scaled_right,
        cv::BORDER_REPLICATE);
    return publish_normalized_seam(path, normalized, effective_canvas_width, effective_canvas_height);
  } catch (const cv::Exception& exception) {
    return absl::ResourceExhaustedError("Unable to normalize seam: " + std::string(exception.what()));
  } catch (const std::bad_alloc&) {
    return absl::ResourceExhaustedError("Unable to allocate normalized seam");
  }
}

absl::Status validate_remaps(
    const fs::path& directory,
    int index,
    const TiffPlacement& placement,
    const std::pair<int, int>& source_size,
    const std::optional<size_t>& maximum_dimension) {
  const std::string prefix = "mapping_" + std::string(index == 0 ? "0000" : "0001");
  const fs::path x_path = directory / (prefix + "_x.tif");
  const fs::path y_path = directory / (prefix + "_y.tif");
  auto status = inspect_remap_tiff(x_path, placement, maximum_dimension);
  if (!status.ok())
    return status;
  status = inspect_remap_tiff(y_path, placement, maximum_dimension);
  if (!status.ok())
    return status;
  cv::Mat x;
  cv::Mat y;
  try {
    x = cv::imread(x_path.string(), cv::IMREAD_ANYDEPTH | cv::IMREAD_GRAYSCALE);
    y = cv::imread(y_path.string(), cv::IMREAD_ANYDEPTH | cv::IMREAD_GRAYSCALE);
  } catch (const cv::Exception& exception) {
    return absl::ResourceExhaustedError("Unable to safely decode Hugin remaps: " + std::string(exception.what()));
  } catch (const std::bad_alloc&) {
    return absl::ResourceExhaustedError("Unable to allocate decoded Hugin remaps");
  }
  if (x.empty() || y.empty() || x.type() != CV_16UC1 || y.type() != CV_16UC1 || x.size() != y.size() ||
      x.cols != placement.width || x.rows != placement.height) {
    return absl::FailedPreconditionError(
        "Hugin remap TIFFs violate the CV_16U size/type contract for camera " + std::to_string(index));
  }
  size_t valid_count = 0;
  uint16_t minimum_x = std::numeric_limits<uint16_t>::max();
  uint16_t minimum_y = std::numeric_limits<uint16_t>::max();
  uint16_t maximum_x = 0;
  uint16_t maximum_y = 0;
  constexpr uint16_t unmapped = std::numeric_limits<uint16_t>::max();
  for (int row = 0; row < x.rows; ++row) {
    const uint16_t* x_values = x.ptr<uint16_t>(row);
    const uint16_t* y_values = y.ptr<uint16_t>(row);
    for (int column = 0; column < x.cols; ++column) {
      const bool x_unmapped = x_values[column] == unmapped;
      const bool y_unmapped = y_values[column] == unmapped;
      if (x_unmapped != y_unmapped) {
        return absl::FailedPreconditionError("Hugin remap has inconsistent unmapped coordinates");
      }
      if (x_unmapped)
        continue;
      if (x_values[column] >= source_size.first || y_values[column] >= source_size.second) {
        return absl::FailedPreconditionError("Hugin remap coordinate lies outside its source image");
      }
      ++valid_count;
      minimum_x = std::min(minimum_x, x_values[column]);
      maximum_x = std::max(maximum_x, x_values[column]);
      minimum_y = std::min(minimum_y, y_values[column]);
      maximum_y = std::max(maximum_y, y_values[column]);
    }
  }
  const size_t minimum_valid = std::max<size_t>(16, x.total() / 1000);
  const uint16_t minimum_x_span = static_cast<uint16_t>(std::min(16, std::max(0, source_size.first - 1)));
  const uint16_t minimum_y_span = static_cast<uint16_t>(std::min(16, std::max(0, source_size.second - 1)));
  if (valid_count < minimum_valid || maximum_x - minimum_x < minimum_x_span || maximum_y - minimum_y < minimum_y_span) {
    return absl::FailedPreconditionError(
        "Hugin remap coordinate coverage is empty or geometrically degenerate for camera " + std::to_string(index));
  }
  return absl::OkStatus();
}

absl::StatusOr<std::pair<int, int>> normalize_and_measure(TiffPlacement* first, TiffPlacement* second) {
  const float minimum_x = std::min(first->x, second->x);
  const float minimum_y = std::min(first->y, second->y);
  first->x -= minimum_x;
  second->x -= minimum_x;
  first->y -= minimum_y;
  second->y -= minimum_y;
  const float width = std::max(first->x + first->width, second->x + second->width);
  const float height = std::max(first->y + first->height, second->y + second->height);
  if (!std::isfinite(width) || !std::isfinite(height) || width < 1.0 || height < 1.0 ||
      width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max()) {
    return absl::FailedPreconditionError("Hugin mapping TIFFs produce an invalid canvas");
  }
  return std::make_pair(static_cast<int>(width), static_cast<int>(height));
}

absl::Status create_hard_seam(
    const fs::path& path,
    const TiffPlacement& first,
    const TiffPlacement& second,
    int canvas_width,
    int canvas_height) {
  const int x0 = static_cast<int>(first.x);
  const int y0 = static_cast<int>(first.y);
  const int x1 = static_cast<int>(second.x);
  const int y1 = static_cast<int>(second.y);
  const int x0_end = x0 + first.width;
  const int x1_end = x1 + second.width;
  const int overlap_start = std::max(x0, x1);
  const int overlap_end = std::min(x0_end, x1_end);
  int seam_x = canvas_width / 2;
  if (overlap_end > overlap_start)
    seam_x = overlap_start + (overlap_end - overlap_start) / 2;
  else if (x1 > x0)
    seam_x = x1;
  seam_x = std::clamp(seam_x, 0, canvas_width);
  cv::Mat seam(canvas_height, canvas_width, CV_8U, cv::Scalar(0));
  if (seam_x < canvas_width)
    seam.colRange(seam_x, canvas_width).setTo(255);
  const int y0_end = y0 + first.height;
  const int y1_end = y1 + second.height;
  for (int y = 0; y < canvas_height; ++y) {
    const bool in0 = y >= y0 && y < y0_end;
    const bool in1 = y >= y1 && y < y1_end;
    if (in0 && !in1) {
      std::fill(
          seam.ptr<uint8_t>(y) + std::clamp(x0, 0, canvas_width),
          seam.ptr<uint8_t>(y) + std::clamp(x0_end, 0, canvas_width),
          static_cast<uint8_t>(0));
    } else if (in1 && !in0) {
      std::fill(
          seam.ptr<uint8_t>(y) + std::clamp(x1, 0, canvas_width),
          seam.ptr<uint8_t>(y) + std::clamp(x1_end, 0, canvas_width),
          static_cast<uint8_t>(255));
    }
  }
  if (!cv::imwrite(path.string(), seam))
    return absl::InternalError("Unable to write validated fallback Hugin seam: " + path.string());
  return absl::OkStatus();
}

absl::StatusOr<std::pair<int, int>> measure_staged_remap_canvas(
    const fs::path& directory,
    const std::optional<size_t>& maximum_dimension) {
  auto first_result = read_tiff_placement(directory / "mapping_0000.tif", maximum_dimension);
  auto second_result = read_tiff_placement(directory / "mapping_0001.tif", maximum_dimension);
  if (!first_result.ok())
    return first_result.status();
  if (!second_result.ok())
    return second_result.status();
  TiffPlacement first = *first_result;
  TiffPlacement second = *second_result;
  return normalize_and_measure(&first, &second);
}

absl::Status validate_staged_artifacts(
    const fs::path& directory,
    const std::optional<size_t>& maximum_dimension,
    const std::optional<size_t>& max_output_width) {
  for (const std::string& artifact : required_stitch_artifact_names()) {
    auto status = validate_nonempty_file(directory / artifact);
    if (!status.ok())
      return status;
  }
  auto project = read_file(directory / "autooptimiser_out.pto");
  if (!project.ok())
    return project.status();
  auto project_canvas = HuginProject::ParseCanvasSize(*project);
  auto projection = HuginProject::ParseProjection(*project);
  if (!project_canvas.ok() || !projection.ok())
    return absl::FailedPreconditionError("Hugin optimized project has an invalid canvas or projection");

  auto first_result = read_tiff_placement(directory / "mapping_0000.tif", maximum_dimension);
  auto second_result = read_tiff_placement(directory / "mapping_0001.tif", maximum_dimension);
  if (!first_result.ok())
    return first_result.status();
  if (!second_result.ok())
    return second_result.status();
  TiffPlacement first = *first_result;
  TiffPlacement second = *second_result;
  auto canvas = normalize_and_measure(&first, &second);
  if (!canvas.ok())
    return canvas.status();
  auto status = validate_decoded_dimensions(
      static_cast<uint64_t>(canvas->first),
      static_cast<uint64_t>(canvas->second),
      maximum_dimension,
      "Decoded Hugin remap canvas");
  if (!status.ok())
    return status;
  if (max_output_width.has_value() && *max_output_width > 0 && static_cast<size_t>(canvas->first) > *max_output_width) {
    return absl::FailedPreconditionError("Decoded Hugin remap canvas exceeds the configured maximum output width");
  }
  auto first_source_size = read_png_dimensions(directory / "left.png");
  auto second_source_size = read_png_dimensions(directory / "right.png");
  if (!first_source_size.ok())
    return first_source_size.status();
  if (!second_source_size.ok())
    return second_source_size.status();
  status = validate_remaps(directory, 0, first, *first_source_size, maximum_dimension);
  if (!status.ok())
    return status;
  status = validate_remaps(directory, 1, second, *second_source_size, maximum_dimension);
  if (!status.ok())
    return status;
  // Nona emits cropped remaps, so their decoded ControlMasks canvas is not
  // expected to equal the uncropped PTO panorama canvas. Both contracts must
  // independently be finite and bounded.
  const fs::path seam_path = directory / "seam_file.png";
  status = validate_and_normalize_seam(seam_path, canvas->first, canvas->second);
  if (!status.ok()) {
    if (!absl::IsFailedPrecondition(status))
      return status;
    if (!hard_seam_fallback_enabled()) {
      return absl::FailedPreconditionError(
          "enblend did not produce a usable seam_file.png; refusing to generate a hard-seam fallback: " +
          status.ToString() +
          ". Fix enblend seam generation or set HM_ALLOW_HARD_SEAM_FALLBACK=1 to explicitly allow the fallback");
    }
    try {
      status = create_hard_seam(seam_path, first, second, canvas->first, canvas->second);
    } catch (const cv::Exception& exception) {
      return absl::ResourceExhaustedError("Unable to safely generate Hugin seam: " + std::string(exception.what()));
    } catch (const std::bad_alloc&) {
      return absl::ResourceExhaustedError("Unable to allocate generated Hugin seam");
    }
    if (!status.ok())
      return status;
    status = validate_and_normalize_seam(seam_path, canvas->first, canvas->second);
    if (!status.ok())
      return status;
  }
  return fsync_stitch_path(seam_path);
}

absl::StatusOr<fs::path> make_staging_directory(const fs::path& game_dir) {
  std::string pattern = (game_dir / ".hstream-stitch-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  char* created = ::mkdtemp(writable.data());
  if (created == nullptr) {
    return absl::InternalError(
        "Unable to create stitch staging directory under " + game_dir.string() + ": " + std::strerror(errno));
  }
  if (::chmod(created, 0700) != 0) {
    std::error_code ignored;
    fs::remove_all(created, ignored);
    return absl::InternalError("Unable to make stitch staging directory private");
  }
  return fs::path(created);
}

void remove_mapping_outputs(const fs::path& directory) {
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(directory, error)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("mapping_", 0) == 0 && (entry.path().extension() == ".tif" || entry.path().extension() == ".tiff")) {
      fs::remove(entry.path(), error);
      error.clear();
    }
  }
}

absl::Status run_autooptimiser(
    const std::string& autooptimiser,
    const fs::path& directory,
    const std::optional<double>& output_scale = std::nullopt,
    const std::function<bool()>& is_cancelled = {}) {
  // Match HockeyMOM's automatic alignment path: let Hugin choose the
  // optimization stages, projection, field of view, and canvas. When the
  // resulting canvas exceeds the configured GPU limit, -x scales that same
  // automatic result without requiring a separate pano_modify pass.
  std::vector<std::string> command = {autooptimiser, "-a", "-l", "-s", "-q"};
  if (output_scale.has_value()) {
    if (!std::isfinite(*output_scale) || *output_scale <= 0.0)
      return absl::InvalidArgumentError("Autooptimiser output scale must be positive and finite");
    std::ostringstream scale;
    scale.imbue(std::locale::classic());
    scale << std::setprecision(12) << *output_scale;
    command.insert(command.end(), {"-x", scale.str()});
  }
  command.insert(command.end(), {"-o", "autooptimiser_out.pto", "hm_project.pto"});
  std::string output;
  auto status = run_checked(command, directory, &output, is_cancelled);
  if (!status.ok())
    return status;
  static const std::regex rms_pattern(R"(([0-9]+(?:[.][0-9]+)?(?:[eE][+-]?[0-9]+)?)[[:space:]]+units)");
  double rms = std::numeric_limits<double>::quiet_NaN();
  for (std::sregex_iterator match(output.begin(), output.end(), rms_pattern), end; match != end; ++match)
    rms = std::stod((*match)[1].str());
  if (!std::isfinite(rms))
    return absl::FailedPreconditionError("Unable to parse Hugin control-point optimization RMS");
  if (rms > kMaximumOptimizationRmsPixels) {
    return absl::FailedPreconditionError(
        "Hugin control-point optimization RMS is too large: " + std::to_string(rms) + " pixels");
  }
  return absl::OkStatus();
}

absl::Status run_nona(
    const std::string& nona,
    const fs::path& directory,
    const std::function<bool()>& is_cancelled = {}) {
  remove_mapping_outputs(directory);
  return run_checked(
      {nona, "-m", "TIFF_m", "-z", "NONE", "--bigtiff", "-c", "-o", "mapping_", "autooptimiser_out.pto"},
      directory,
      nullptr,
      is_cancelled);
}

absl::Status publish_artifacts(const fs::path& staging, const fs::path& game_dir, bool* prepared) {
  const std::vector<std::string>& names = stitch_artifact_names();
  const fs::path backups = staging / "previous";
  std::error_code error;
  fs::create_directory(backups, error);
  if (error)
    return absl::InternalError("Unable to prepare stitch artifact rollback directory: " + error.message());
  std::ostringstream manifest;
  std::ostringstream prior_manifest;
  for (const std::string& name : names)
    manifest << name << '\n';
  auto status = write_stitch_transaction_file(staging / "artifacts", manifest.str());
  if (!status.ok())
    return status;
  for (const std::string& name : names) {
    const bool exists = fs::exists(game_dir / name, error);
    if (error)
      return absl::InternalError("Unable to inspect previous stitch artifact " + name + ": " + error.message());
    if (exists) {
      if (!fs::is_regular_file(game_dir / name, error) || error) {
        return absl::FailedPreconditionError("Previous stitch artifact is not a regular file: " + name);
      }
      prior_manifest << name << '\n';
    }
  }
  status = write_stitch_transaction_file(staging / "previous_artifacts", prior_manifest.str());
  if (!status.ok())
    return status;
  status = fsync_stitch_path(backups, true);
  if (!status.ok())
    return status;
  status = fsync_stitch_path(staging, true);
  if (!status.ok())
    return status;
  status = publish_transaction_state(staging, "PREPARED\n");
  if (!status.ok())
    return status;
  // The PREPARED state is only recoverable after the staging directory entry
  // itself is durable in its parent. Do this before changing any flat
  // artifacts in the game directory.
  status = fsync_stitch_path(game_dir, true);
  if (!status.ok())
    return status;
  *prepared = true;
  if (const char* interrupt = std::getenv("HM_TEST_STITCH_INTERRUPT_AFTER_PREPARE_SYNC");
      interrupt != nullptr && std::string(interrupt) == "1") {
    return absl::InternalError("Injected stitch interruption after durable preparation");
  }

  auto rollback_error = [&](const std::string& message) {
    const auto rollback_status = recover_stitch_transactions_locked(game_dir);
    if (!rollback_status.ok())
      return absl::InternalError(message + "; rollback also failed: " + std::string(rollback_status.message()));
    return absl::InternalError(message);
  };
  status = publish_transaction_state(staging, "BACKING_UP\n");
  if (!status.ok())
    return status;
  size_t backup_count = 0;
  for (const std::string& name : names) {
    const bool exists = fs::exists(game_dir / name, error);
    if (error)
      return rollback_error("Unable to inspect old stitch artifact " + name + ": " + error.message());
    if (!exists)
      continue;
    fs::rename(game_dir / name, backups / name, error);
    if (error)
      return rollback_error("Unable to preserve previous stitch artifact " + name + ": " + error.message());
    ++backup_count;
    if (backup_count == 1) {
      if (const char* interrupt = std::getenv("HM_TEST_STITCH_INTERRUPT_DURING_BACKUP");
          interrupt != nullptr && std::string(interrupt) == "1") {
        return absl::InternalError("Injected stitch interruption during artifact backup");
      }
    }
  }
  status = fsync_stitch_path(backups, true);
  if (!status.ok())
    return rollback_error(std::string(status.message()));
  status = fsync_stitch_path(game_dir, true);
  if (!status.ok())
    return rollback_error(std::string(status.message()));
  status = publish_transaction_state(staging, "BACKED_UP\n");
  if (!status.ok())
    return rollback_error(std::string(status.message()));
  if (const char* interrupt = std::getenv("HM_TEST_STITCH_INTERRUPT_AFTER_BACKUP_SYNC");
      interrupt != nullptr && std::string(interrupt) == "1") {
    return absl::InternalError("Injected stitch interruption after durable backup");
  }
  for (const std::string& name : names) {
    if (!fs::exists(staging / name, error)) {
      error.clear();
      continue;
    }
    fs::rename(staging / name, game_dir / name, error);
    if (error)
      return rollback_error("Unable to publish stitch artifact " + name + ": " + error.message());
    status = fsync_stitch_path(game_dir / name);
    if (!status.ok())
      return rollback_error(std::string(status.message()));
  }
  status = fsync_stitch_path(game_dir, true);
  if (!status.ok())
    return rollback_error(std::string(status.message()));
  status = write_stitch_transaction_file(staging / "state.committed", "COMMITTED\n");
  if (!status.ok())
    return rollback_error(std::string(status.message()));
  fs::rename(staging / "state.committed", staging / "state", error);
  if (error)
    return rollback_error("Unable to commit stitch transaction: " + error.message());
  status = fsync_stitch_path(staging, true);
  if (!status.ok())
    return status;
  fs::remove_all(staging, error);
  if (error)
    return absl::InternalError("Unable to clean committed stitch transaction: " + error.message());
  status = fsync_stitch_path(game_dir, true);
  if (!status.ok())
    return status;
  return absl::OkStatus();
}

} // namespace

HuginProject::ArtifactLock::~ArtifactLock() {
  if (descriptor_ >= 0) {
    ::flock(descriptor_, LOCK_UN);
    ::close(descriptor_);
  }
}

absl::StatusOr<std::unique_ptr<HuginProject::ArtifactLock>> HuginProject::RecoverAndLock(const fs::path& game_dir) {
  std::error_code error;
  if (!fs::is_directory(game_dir, error) || error)
    return absl::NotFoundError("Unable to recover Hugin artifacts outside an existing game directory");
  auto descriptor = lock_stitch_transactions(game_dir);
  if (!descriptor.ok())
    return descriptor.status();
  auto lock = std::unique_ptr<ArtifactLock>(new ArtifactLock(*descriptor));
  auto recovery = recover_stitch_transactions_locked(game_dir);
  if (!recovery.ok())
    return recovery;
  return std::move(lock);
}

absl::Status HuginProject::Recover(const fs::path& game_dir) {
  auto lock = RecoverAndLock(game_dir);
  return lock.ok() ? absl::OkStatus() : lock.status();
}

absl::Status HuginProject::ValidateAndNormalizeSeam(const fs::path& seam_path, int canvas_width, int canvas_height) {
  return validate_and_normalize_seam(seam_path, canvas_width, canvas_height);
}

absl::Status HuginProject::ValidateAndNormalizeSeam(
    const fs::path& seam_path,
    int native_canvas_width,
    int native_canvas_height,
    int effective_canvas_width,
    int effective_canvas_height) {
  const double scale = static_cast<double>(effective_canvas_width) / static_cast<double>(native_canvas_width);
  return validate_and_normalize_seam(
      seam_path, native_canvas_width, native_canvas_height, effective_canvas_width, effective_canvas_height, scale);
}

absl::Status HuginProject::ValidateAndNormalizeSeam(
    const fs::path& seam_path,
    int native_canvas_width,
    int native_canvas_height,
    int effective_canvas_width,
    int effective_canvas_height,
    double scale) {
  return validate_and_normalize_seam(
      seam_path, native_canvas_width, native_canvas_height, effective_canvas_width, effective_canvas_height, scale);
}

absl::Status HuginProject::ValidateSeamLayout(
    const fs::path& seam_path,
    int native_canvas_width,
    int native_canvas_height,
    int effective_canvas_width,
    int effective_canvas_height) {
  auto layout = validated_seam_layout(
      seam_path, native_canvas_width, native_canvas_height, effective_canvas_width, effective_canvas_height);
  return layout.ok() ? absl::OkStatus() : layout.status();
}

absl::Status HuginProject::ValidateSeamForConfiguredArtifacts(
    const fs::path& seam_path,
    int native_canvas_width,
    int native_canvas_height,
    int effective_canvas_width,
    int effective_canvas_height) {
  return validate_seam_for_configured_artifacts(
      seam_path, native_canvas_width, native_canvas_height, effective_canvas_width, effective_canvas_height);
}

absl::StatusOr<std::string> HuginProject::GenerationId(const fs::path& game_dir, const ArtifactLock&) {
  return stitch_artifact_generation_id_locked(game_dir);
}

absl::StatusOr<std::optional<HuginProject::CanvasProvenance>> HuginProject::ReadCanvasProvenance(
    const fs::path& game_dir,
    const ArtifactLock&) {
  const fs::path path = game_dir / kStitchCanvasProvenanceArtifact;
  std::error_code error;
  if (!fs::exists(path, error)) {
    if (error)
      return absl::InternalError("Unable to inspect stitching canvas provenance: " + error.message());
    return std::nullopt;
  }
  auto contents = read_file(path);
  if (!contents.ok())
    return contents.status();
  CanvasProvenance provenance;
  HM_ASSIGN_OR_RETURN(provenance, parse_canvas_provenance(*contents));
  return provenance;
}

absl::StatusOr<std::string> HuginProject::InsertControlPoints(
    const std::string& pto,
    const std::vector<FeatureMatch>& matches) {
  if (matches.size() < kMinimumUsableMatches) {
    return absl::FailedPreconditionError(
        "Feature matcher produced " + std::to_string(matches.size()) + ", fewer than the required " +
        std::to_string(kMinimumUsableMatches) + " control points");
  }
  std::ostringstream points;
  points.imbue(std::locale::classic());
  points << std::setprecision(std::numeric_limits<float>::max_digits10);
  for (const FeatureMatch& match : matches) {
    if (!std::isfinite(match.left.x) || !std::isfinite(match.left.y) || !std::isfinite(match.right.x) ||
        !std::isfinite(match.right.y) || match.left.x < 0.0f || match.left.y < 0.0f || match.right.x < 0.0f ||
        match.right.y < 0.0f) {
      return absl::InvalidArgumentError("Control points must contain finite non-negative coordinates");
    }
    points << "c n0 N1 x" << match.left.x << " y" << match.left.y << " X" << match.right.x << " Y" << match.right.y
           << " t0\n";
  }

  std::istringstream input(pto);
  std::ostringstream output;
  std::string line;
  bool marker_seen = false;
  while (std::getline(input, line)) {
    if (line.rfind("c ", 0) == 0)
      continue;
    output << line << '\n';
    if (!marker_seen && line == "# control points") {
      output << points.str();
      marker_seen = true;
    }
  }
  if (!marker_seen)
    return absl::InvalidArgumentError("Hugin PTO has no control-point marker");
  return output.str();
}

absl::StatusOr<std::pair<size_t, size_t>> HuginProject::ParseCanvasSize(const std::string& pto) {
  static const std::regex width_pattern(R"((?:^|[[:space:]])w([0-9]+)(?:[[:space:]]|$))");
  static const std::regex height_pattern(R"((?:^|[[:space:]])h([0-9]+)(?:[[:space:]]|$))");
  std::istringstream input(pto);
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("p ", 0) != 0)
      continue;
    std::smatch width_match;
    std::smatch height_match;
    if (!std::regex_search(line, width_match, width_pattern) ||
        !std::regex_search(line, height_match, height_pattern)) {
      return absl::InvalidArgumentError("Hugin panorama line has no valid canvas dimensions");
    }
    try {
      const size_t width = std::stoull(width_match[1].str());
      const size_t height = std::stoull(height_match[1].str());
      if (width == 0 || height == 0)
        throw std::out_of_range("zero canvas");
      return std::make_pair(width, height);
    } catch (const std::exception&) {
      return absl::InvalidArgumentError("Hugin panorama canvas dimensions are invalid");
    }
  }
  return absl::InvalidArgumentError("Hugin PTO has no panorama line");
}

absl::StatusOr<int> HuginProject::ParseProjection(const std::string& pto) {
  std::istringstream input(pto);
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("p ", 0) != 0)
      continue;
    std::istringstream tokens(line);
    std::string token;
    while (tokens >> token) {
      if (token.size() < 2 || token[0] != 'f')
        continue;
      try {
        size_t parsed = 0;
        const int projection = std::stoi(token.substr(1), &parsed);
        if (parsed == token.size() - 1 && projection >= 0)
          return projection;
      } catch (const std::exception&) {
        continue;
      }
    }
    return absl::InvalidArgumentError("Hugin panorama line has no valid projection");
  }
  return absl::InvalidArgumentError("Hugin PTO has no panorama line");
}

absl::StatusOr<HuginProject::CameraPose> HuginProject::ParseCameraPose(const std::string& pto, size_t image_index) {
  std::istringstream input(pto);
  std::string line;
  size_t current_image = 0;
  while (std::getline(input, line)) {
    if (line.rfind("i ", 0) != 0)
      continue;
    if (current_image++ != image_index)
      continue;

    CameraPose pose{};
    bool found_roll = false;
    bool found_pitch = false;
    bool found_yaw = false;
    std::istringstream tokens(line);
    std::string token;
    while (tokens >> token) {
      if (token.size() < 2 || (token[0] != 'r' && token[0] != 'p' && token[0] != 'y') || token[1] == '=')
        continue;
      try {
        size_t parsed = 0;
        const double value = std::stod(token.substr(1), &parsed);
        if (parsed != token.size() - 1 || !std::isfinite(value))
          continue;
        if (token[0] == 'r') {
          pose.roll = value;
          found_roll = true;
        } else if (token[0] == 'p') {
          pose.pitch = value;
          found_pitch = true;
        } else {
          pose.yaw = value;
          found_yaw = true;
        }
      } catch (const std::exception&) {
        continue;
      }
    }
    if (!found_roll || !found_pitch || !found_yaw)
      return absl::InvalidArgumentError("Hugin image line has no finite roll/pitch/yaw pose");
    return pose;
  }
  return absl::InvalidArgumentError("Hugin PTO has no requested image line");
}

absl::Status HuginProject::Configure(
    const fs::path& game_dir,
    const std::vector<FeatureMatch>& matches,
    const Options& options) {
  if (options.is_cancelled && options.is_cancelled()) {
    return absl::CancelledError("Hugin calibration cancelled before optimizer setup");
  }
  return Configure(game_dir, game_dir / "left.png", game_dir / "right.png", matches, options);
}

absl::Status HuginProject::Configure(
    const fs::path& game_dir,
    const fs::path& left_image,
    const fs::path& right_image,
    const std::vector<FeatureMatch>& matches,
    const Options& options) {
  if (options.is_cancelled && options.is_cancelled()) {
    return absl::CancelledError("Hugin calibration cancelled before optimizer setup");
  }
  if (!std::isfinite(options.horizontal_fov) || options.horizontal_fov <= 0.0 || options.horizontal_fov >= 360.0) {
    return absl::InvalidArgumentError("Hugin horizontal field of view must be between 0 and 360 degrees");
  }
  if (matches.size() < kMinimumUsableMatches) {
    return absl::FailedPreconditionError("Insufficient control points for Hugin optimization");
  }
  for (const fs::path& image : {left_image, right_image}) {
    auto status = validate_nonempty_file(image);
    if (!status.ok())
      return status;
  }
  if (options.progress)
    options.progress("optimizer", "started", "Preparing and running panorama optimizer (autooptimiser)");
  auto transaction_lock = RecoverAndLock(game_dir);
  if (!transaction_lock.ok())
    return transaction_lock.status();

  fs::path staging;
  auto staging_result = make_staging_directory(game_dir);
  if (!staging_result.ok())
    return staging_result.status();
  staging = *staging_result;
  struct Cleanup {
    fs::path path;
    bool prepared{false};
    ~Cleanup() {
      if (prepared)
        return;
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{staging};

  std::error_code error;
  fs::copy_file(left_image, staging / "left.png", fs::copy_options::overwrite_existing, error);
  if (error)
    return absl::InternalError("Unable to stage left image: " + error.message());
  fs::copy_file(right_image, staging / "right.png", fs::copy_options::overwrite_existing, error);
  if (error)
    return absl::InternalError("Unable to stage right image: " + error.message());

  auto pto_gen = executable("HM_PTO_GEN", "pto_gen");
  if (!pto_gen.ok())
    return pto_gen.status();

  std::ostringstream fov;
  fov.imbue(std::locale::classic());
  fov << std::setprecision(12) << options.horizontal_fov;
  // The camera frames are rectilinear images. Declaring them cylindrical here
  // produces a superficially blendable panorama, but leaves a large geometric
  // mismatch in the overlap that GPU blending exposes as repeated/ghosted
  // content. Keep this aligned with HockeyMOM's proven stitching setup.
  auto status = run_checked(
      {*pto_gen, "-p", "0", "-o", "hm_project.pto", "-f", fov.str(), "left.png", "right.png"},
      staging,
      nullptr,
      options.is_cancelled);
  if (!status.ok())
    return status;
  auto project = read_file(staging / "hm_project.pto");
  if (!project.ok())
    return project.status();
  auto with_points = InsertControlPoints(*project, matches);
  if (!with_points.ok())
    return with_points.status();
  status = write_file(staging / "hm_project.pto", *with_points);
  if (!status.ok())
    return status;

  std::optional<std::string> autooptimiser_path;
  if (options.mapping_backend == MappingBackend::kNona) {
    auto autooptimiser = executable("HM_AUTOOPTIMISER", "autooptimiser");
    if (!autooptimiser.ok())
      return autooptimiser.status();
    autooptimiser_path = *autooptimiser;
    status = run_autooptimiser(*autooptimiser_path, staging, std::nullopt, options.is_cancelled);
    if (!status.ok())
      return status;
    if (options.progress)
      options.progress("optimizer", "complete", "Panorama alignment optimized");
  } else {
    fs::copy_file(
        staging / "hm_project.pto", staging / "autooptimiser_out.pto", fs::copy_options::overwrite_existing, error);
    if (error)
      return absl::InternalError("Unable to copy native OpenCV project file: " + error.message());
    if (options.progress)
      options.progress("optimizer", "complete", "Native OpenCV mapping does not require Hugin optimization");
  }
  if (options.progress)
    options.progress("canvas", "started", "Building stitch maps and panorama preview");
  std::optional<double> output_scale;
  std::pair<size_t, size_t> source_canvas{0, 0};
  bool max_output_width_applied = false;
  bool max_canvas_dimension_applied = false;
  auto fit_canvas = [&](size_t width, size_t height, double factor, double rounding_guard) -> absl::Status {
    (void)width;
    (void)height;
    output_scale = output_scale.value_or(1.0) * factor * rounding_guard;
    return run_autooptimiser(*autooptimiser_path, staging, output_scale, options.is_cancelled);
  };
  auto fit_canvas_longest = [&](size_t width, size_t height, double rounding_guard) -> absl::Status {
    const size_t longest = std::max(width, height);
    const double factor = static_cast<double>(*options.max_canvas_dimension) / static_cast<double>(longest);
    max_canvas_dimension_applied = true;
    return fit_canvas(width, height, factor, rounding_guard);
  };
  auto fit_canvas_width = [&](size_t width, size_t height, double rounding_guard) -> absl::Status {
    if (!options.max_output_width.has_value() || *options.max_output_width == 0 || width <= *options.max_output_width)
      return absl::OkStatus();
    const double factor = static_cast<double>(*options.max_output_width) / static_cast<double>(width);
    max_output_width_applied = true;
    return fit_canvas(width, height, factor, rounding_guard);
  };
  if (options.mapping_backend != MappingBackend::kNona && output_scale.has_value()) {
    return absl::InvalidArgumentError("Native OpenCV mapping backends do not accept Hugin output scaling");
  }
  if (options.max_canvas_dimension.has_value() || options.max_output_width.has_value()) {
    auto optimized = read_file(staging / "autooptimiser_out.pto");
    if (!optimized.ok())
      return optimized.status();
    auto dimensions = ParseCanvasSize(*optimized);
    if (!dimensions.ok())
      return dimensions.status();
    source_canvas = *dimensions;
    if (options.mapping_backend == MappingBackend::kNona) {
      if (options.max_output_width.has_value() && dimensions->first > *options.max_output_width) {
        status = fit_canvas_width(dimensions->first, dimensions->second, 1.0);
        if (!status.ok())
          return status;
        optimized = read_file(staging / "autooptimiser_out.pto");
        if (!optimized.ok())
          return optimized.status();
        dimensions = ParseCanvasSize(*optimized);
        if (!dimensions.ok())
          return dimensions.status();
      }
      if (options.max_canvas_dimension.has_value()) {
        const size_t longest = std::max(dimensions->first, dimensions->second);
        if (longest > *options.max_canvas_dimension) {
          status = fit_canvas_longest(dimensions->first, dimensions->second, 1.0);
          if (!status.ok())
            return status;
        }
      }
    }
  }

  if (options.mapping_backend == MappingBackend::kNona) {
    auto nona = executable("HM_NONA", "nona");
    if (!nona.ok())
      return nona.status();
    for (int attempt = 0; attempt < 3; ++attempt) {
      status = run_nona(*nona, staging, options.is_cancelled);
      if (!status.ok())
        return status;
      bool mappings_valid = true;
      const auto& required_artifacts = required_stitch_artifact_names();
      for (size_t index = 2; index + 1 < required_artifacts.size(); ++index) {
        status = validate_nonempty_file(staging / required_artifacts[index]);
        if (!status.ok()) {
          mappings_valid = false;
          break;
        }
      }
      if (!mappings_valid)
        return status;
      if (!options.max_canvas_dimension.has_value() && !options.max_output_width.has_value())
        break;

      // Validate the actual TIFF placement canvas because nona's cropped remaps
      // are the contract hm-cupano loads at runtime.
      auto optimized = read_file(staging / "autooptimiser_out.pto");
      if (!optimized.ok())
        return optimized.status();
      auto dimensions = ParseCanvasSize(*optimized);
      if (!dimensions.ok())
        return dimensions.status();
      auto remap_canvas = measure_staged_remap_canvas(staging, std::nullopt);
      if (!remap_canvas.ok())
        return remap_canvas.status();
      // Nona's cropped TIFF placement canvas can round above the PTO canvas.
      // Record the equivalent unconstrained extent before applying a retry so
      // later compatibility checks distinguish caps that produce different
      // measured remap scales.
      const long double applied_scale = output_scale.value_or(1.0);
      if (!std::isfinite(static_cast<double>(applied_scale)) || applied_scale <= 0.0L)
        return absl::FailedPreconditionError("Invalid cumulative Hugin output scale");
      const auto unconstrained_extent = [&](int measured) {
        const long double unscaled = static_cast<long double>(measured) / applied_scale;
        return static_cast<size_t>(std::ceil(unscaled - 1e-12L));
      };
      if (attempt == 0) {
        source_canvas.first = std::max(source_canvas.first, unconstrained_extent(remap_canvas->first));
        source_canvas.second = std::max(source_canvas.second, unconstrained_extent(remap_canvas->second));
      }
      const bool width_ok = !options.max_output_width.has_value() ||
          static_cast<size_t>(remap_canvas->first) <= *options.max_output_width;
      const bool dimension_ok = !options.max_canvas_dimension.has_value() ||
          static_cast<size_t>(std::max(remap_canvas->first, remap_canvas->second)) <= *options.max_canvas_dimension;
      if (width_ok && dimension_ok)
        break;
      if (attempt == 2) {
        return absl::FailedPreconditionError("Hugin mapping canvas still exceeds requested size after three attempts");
      }
      if (!width_ok) {
        status = fit_canvas_width(
            static_cast<size_t>(remap_canvas->first), static_cast<size_t>(remap_canvas->second), 0.999);
      } else {
        status = fit_canvas_longest(
            static_cast<size_t>(remap_canvas->first), static_cast<size_t>(remap_canvas->second), 0.999);
      }
      if (!status.ok())
        return status;
    }
  } else {
    const cv::Mat left = cv::imread((staging / "left.png").string(), cv::IMREAD_COLOR);
    const cv::Mat right = cv::imread((staging / "right.png").string(), cv::IMREAD_COLOR);
    auto maps = CreateOpenCvMappingFiles(
        staging, left, right, matches, options.mapping_backend, options.max_canvas_dimension, options.max_output_width);
    if (!maps.ok())
      return maps.status();
    source_canvas = {maps->source_canvas_width, maps->source_canvas_height};
    max_output_width_applied = maps->max_output_width_applied;
    max_canvas_dimension_applied = maps->max_canvas_dimension_applied;
    std::cout << "OpenCV mapping backend " << MappingBackendName(options.mapping_backend) << " generated "
              << maps->canvas_width << "x" << maps->canvas_height << " canvas with " << maps->inlier_count
              << " fitted control points" << std::endl;
  }

  // Enblend's preview/seam is required by default for both Hugin and native
  // OpenCV maps. The complete decoded artifact validator may create a hard-seam
  // fallback only when explicitly enabled for diagnostics.
  auto enblend = executable("HM_ENBLEND", "enblend");
  if (enblend.ok()) {
    status = run_checked(
        {*enblend, "--save-masks=seam_file.png", "-o", "panorama.tif", "mapping_0000.tif", "mapping_0001.tif"},
        staging,
        nullptr,
        options.is_cancelled);
    if (!status.ok()) {
      if (absl::IsCancelled(status))
        return status;
      if (!hard_seam_fallback_enabled()) {
        return absl::FailedPreconditionError(
            "enblend failed to generate seam_file.png and hard-seam fallback is disabled: " + status.ToString() +
            ". Fix enblend or set HM_ALLOW_HARD_SEAM_FALLBACK=1 to explicitly allow the fallback");
      }
      std::cerr << "Warning: native enblend preview failed; HM_ALLOW_HARD_SEAM_FALLBACK=1 permits a hard seam: "
                << status << '\n';
      fs::remove(staging / "seam_file.png", error);
      error.clear();
      fs::remove(staging / "panorama.tif", error);
      error.clear();
    }
  } else if (!hard_seam_fallback_enabled()) {
    return absl::FailedPreconditionError(
        "Cannot generate seam_file.png because enblend is unavailable and hard-seam fallback is disabled: " +
        enblend.status().ToString() +
        ". Install/fix enblend or set HM_ALLOW_HARD_SEAM_FALLBACK=1 to explicitly allow the fallback");
  } else {
    std::cerr << "Warning: enblend is unavailable; HM_ALLOW_HARD_SEAM_FALLBACK=1 permits a hard seam: "
              << enblend.status() << '\n';
  }

  auto published_canvas = measure_staged_remap_canvas(staging, std::nullopt);
  if (!published_canvas.ok())
    return published_canvas.status();
  if (source_canvas.first == 0 || source_canvas.second == 0) {
    source_canvas = {static_cast<size_t>(published_canvas->first), static_cast<size_t>(published_canvas->second)};
  }
  std::ostringstream provenance;
  provenance << "version=2\n"
             << "max-output-width=" << options.max_output_width.value_or(0) << '\n'
             << "max-canvas-dimension=" << options.max_canvas_dimension.value_or(0) << '\n'
             << "source-canvas-width=" << source_canvas.first << '\n'
             << "source-canvas-height=" << source_canvas.second << '\n'
             << "canvas-width=" << published_canvas->first << '\n'
             << "canvas-height=" << published_canvas->second << '\n'
             << "max-output-width-applied=" << (max_output_width_applied ? 1 : 0) << '\n'
             << "max-canvas-dimension-applied=" << (max_canvas_dimension_applied ? 1 : 0) << '\n';
  status = write_file(staging / kStitchCanvasProvenanceArtifact, provenance.str());
  if (!status.ok())
    return status;

  status = validate_staged_artifacts(staging, options.max_canvas_dimension, options.max_output_width);
  if (!status.ok())
    return status;
  if (options.is_cancelled && options.is_cancelled())
    return absl::CancelledError("Hugin calibration cancelled before publication");
  auto config_transaction = GameConfigTransactionLock::Acquire(game_dir);
  if (!config_transaction.ok())
    return config_transaction.status();
  status = validate_no_pending_live_stitched_output_authorization_file_locked(game_dir / "config.yaml");
  if (!status.ok())
    return status;
  if (!options.expected_invalidation_id.empty()) {
    auto validation =
        validate_pending_stitching_invalidation_file_locked(game_dir / "config.yaml", options.expected_invalidation_id);
    if (!validation.ok())
      return validation;
  }
  status = prepare_stitch_generation_publication(staging, game_dir);
  if (!status.ok())
    return status;
  status = publish_artifacts(staging, game_dir, &cleanup.prepared);
  if (status.ok() && options.progress)
    options.progress("canvas", "complete", "Stitch maps and panorama preview are ready");
  return status;
}

} // namespace hm::stitching
