#include "hstream/src/libs/stitching/HuginProject.h"

#include <algorithm>
#include <array>
#include <cerrno>
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
#include <set>
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

extern "C" char** environ;

namespace hm::stitching {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMinimumUsableMatches = 16;
constexpr double kMaximumOptimizationRmsPixels = 50.0;
constexpr size_t kHardMaximumCanvasDimension = 32768;
constexpr uint64_t kHardMaximumCanvasPixels = 128ULL * 1024ULL * 1024ULL;
constexpr const char* kStitchTransactionPrefix = ".hstream-stitch-";

const std::array<const char*, 10> kRequiredArtifacts = {
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
};

// Releases before synchronized inputs were transactionally published used
// this manifest. Accept it during recovery so an interrupted upgrade remains
// recoverable, while new publications include left.png/right.png.
const std::array<const char*, 8> kLegacyRequiredArtifacts = {
    "hm_project.pto",
    "autooptimiser_out.pto",
    "mapping_0000.tif",
    "mapping_0000_x.tif",
    "mapping_0000_y.tif",
    "mapping_0001.tif",
    "mapping_0001_x.tif",
    "mapping_0001_y.tif",
};

const std::array<const char*, 2> kOptionalArtifacts = {"seam_file.png", "panorama.tif"};

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
    std::string* captured = nullptr) {
  const int exit_code = hm::run_command(
      command, working_dir.string(), environment(), [captured](const std::string& error, const std::string& output) {
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
      });
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

std::vector<std::string> artifact_names() {
  std::vector<std::string> names(kRequiredArtifacts.begin(), kRequiredArtifacts.end());
  for (const char* optional : kOptionalArtifacts)
    names.emplace_back(optional);
  return names;
}

std::set<std::string> legacy_artifact_names() {
  std::set<std::string> names(kLegacyRequiredArtifacts.begin(), kLegacyRequiredArtifacts.end());
  names.insert(kOptionalArtifacts.begin(), kOptionalArtifacts.end());
  return names;
}

bool is_artifact_name(const std::string& name) {
  const auto names = artifact_names();
  return std::find(names.begin(), names.end(), name) != names.end();
}

absl::Status fsync_path(const fs::path& path, bool directory = false) {
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

absl::Status copy_file_preserving_mtime(const fs::path& source, const fs::path& destination) {
  std::error_code error;
  const auto modified = fs::last_write_time(source, error);
  if (error)
    return absl::InternalError("Unable to read stitch artifact timestamp: " + error.message());
  fs::copy_file(source, destination, fs::copy_options::overwrite_existing, error);
  if (error)
    return absl::InternalError("Unable to copy stitch artifact: " + error.message());
  fs::last_write_time(destination, modified, error);
  if (error)
    return absl::InternalError("Unable to preserve stitch artifact timestamp: " + error.message());
  return fsync_path(destination);
}

absl::Status write_transaction_file(const fs::path& path, const std::string& contents) {
  auto status = write_file(path, contents);
  if (!status.ok())
    return status;
  return fsync_path(path);
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

absl::StatusOr<std::string> read_transaction_state(const fs::path& transaction, const std::string& transaction_name) {
  const fs::path state_path = transaction / "state";
  std::error_code error;
  const bool state_exists = fs::exists(state_path, error);
  if (error)
    return absl::InternalError("Unable to inspect " + transaction_name + " transaction state: " + error.message());
  if (!state_exists)
    return std::string("UNPREPARED");
  const int descriptor = ::open(state_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0)
    return absl::FailedPreconditionError("Unable to open durable " + transaction_name + " transaction state");
  struct StateFileCleanup {
    int descriptor;
    ~StateFileCleanup() {
      ::close(descriptor);
    }
  } cleanup{descriptor};
  struct stat metadata{};
  if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
      metadata.st_size > 10) {
    return absl::FailedPreconditionError("Invalid durable " + transaction_name + " transaction state file");
  }
  std::string contents(static_cast<size_t>(metadata.st_size), '\0');
  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t count = ::read(descriptor, contents.data() + offset, contents.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return absl::FailedPreconditionError("Unable to read durable " + transaction_name + " transaction state");
    offset += static_cast<size_t>(count);
  }
  if (contents == "PREPARED\n")
    return std::string("PREPARED");
  if (contents == "COMMITTED\n")
    return std::string("COMMITTED");
  return absl::FailedPreconditionError("Invalid durable " + transaction_name + " transaction state contents");
}

absl::Status recover_stitch_transactions_locked(const fs::path& root) {
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(root, error)) {
    if (error)
      return absl::InternalError("Unable to inspect stitch transactions: " + error.message());
    const std::string directory_name = entry.path().filename().string();
    if (!entry.is_directory(error) || error || directory_name.rfind(kStitchTransactionPrefix, 0) != 0) {
      error.clear();
      continue;
    }
    const fs::path transaction = entry.path();
    auto state = read_transaction_state(transaction, "stitch");
    if (!state.ok())
      return state.status();
    if (*state == "PREPARED") {
      std::ifstream manifest(transaction / "artifacts");
      if (!manifest)
        return absl::FailedPreconditionError("Prepared stitch transaction has no artifact manifest");
      std::set<std::string> manifested;
      std::string name;
      while (manifest >> name) {
        if (fs::path(name).filename() != name || !is_artifact_name(name) || !manifested.insert(name).second)
          return absl::InvalidArgumentError("Invalid stitch transaction filename: " + name);
      }
      const std::vector<std::string> current_name_list = artifact_names();
      const std::set<std::string> current_names(current_name_list.begin(), current_name_list.end());
      if (!manifest.eof() || (manifested != current_names && manifested != legacy_artifact_names())) {
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
          if (!old.is_regular_file(error) || error || !is_artifact_name(old_name))
            return absl::InvalidArgumentError("Invalid stitch transaction backup: " + old_name);
          backups.push_back(old.path());
        }
      }
      for (const std::string& artifact : manifested) {
        name = artifact;
        fs::remove(root / name, error);
        if (error)
          return absl::InternalError("Unable to remove interrupted stitch artifact: " + error.message());
      }
      for (const fs::path& old : backups) {
        const fs::path destination = root / old.filename();
        auto status = copy_file_preserving_mtime(old, destination);
        if (!status.ok())
          return absl::InternalError("Unable to restore interrupted stitch artifact: " + std::string(status.message()));
      }
      error.clear();
      auto status = fsync_path(root, true);
      if (!status.ok())
        return status;
    }
    // COMMITTED transactions already have a durable new generation. An
    // UNPREPARED directory has no publication metadata and never changed a
    // root artifact.
    fs::remove_all(transaction, error);
    if (error)
      return absl::InternalError("Unable to clean stitch transaction: " + error.message());
  }
  return fsync_path(root, true);
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
  (void)TIFFGetField(tif, TIFFTAG_XPOSITION, &x_position);
  (void)TIFFGetField(tif, TIFFTAG_YPOSITION, &y_position);
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
  if (!metadata_valid || !pixels_valid || width == 0 || height == 0 || width > std::numeric_limits<int>::max() ||
      height > std::numeric_limits<int>::max() || !std::isfinite(x_resolution) || !std::isfinite(y_resolution) ||
      x_resolution <= 0.0f || y_resolution <= 0.0f || !std::isfinite(x_position) || !std::isfinite(y_position)) {
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

absl::StatusOr<std::pair<int, int>> read_png_dimensions(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::array<unsigned char, 24> header{};
  input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
  const std::array<unsigned char, 8> signature = {137, 80, 78, 71, 13, 10, 26, 10};
  if (input.gcount() != static_cast<std::streamsize>(header.size()) ||
      !std::equal(signature.begin(), signature.end(), header.begin()) || header[8] != 0 || header[9] != 0 ||
      header[10] != 0 || header[11] != 13 || header[12] != 'I' || header[13] != 'H' || header[14] != 'D' ||
      header[15] != 'R') {
    return absl::FailedPreconditionError("Invalid PNG header: " + path.string());
  }
  auto big_endian_u32 = [&](size_t offset) {
    return (static_cast<uint32_t>(header[offset]) << 24) | (static_cast<uint32_t>(header[offset + 1]) << 16) |
        (static_cast<uint32_t>(header[offset + 2]) << 8) | static_cast<uint32_t>(header[offset + 3]);
  };
  const uint32_t width = big_endian_u32(16);
  const uint32_t height = big_endian_u32(20);
  if (width == 0 || height == 0 || width > std::numeric_limits<int>::max() ||
      height > std::numeric_limits<int>::max()) {
    return absl::FailedPreconditionError("Invalid PNG dimensions: " + path.string());
  }
  return std::make_pair(static_cast<int>(width), static_cast<int>(height));
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

absl::Status validate_staged_artifacts(const fs::path& directory, const std::optional<size_t>& maximum_dimension) {
  for (const char* artifact : kRequiredArtifacts) {
    auto status = validate_nonempty_file(directory / artifact);
    if (!status.ok())
      return status;
  }
  auto project = read_file(directory / "autooptimiser_out.pto");
  if (!project.ok())
    return project.status();
  auto project_canvas = HuginProject::ParseCanvasSize(*project);
  auto projection = HuginProject::ParseProjection(*project);
  if (!project_canvas.ok() || !projection.ok() || *projection != 1)
    return absl::FailedPreconditionError("Hugin optimized project has an invalid canvas or projection");

  TiffPlacement first;
  TiffPlacement second;
  auto first_result = read_tiff_placement(directory / "mapping_0000.tif", maximum_dimension);
  auto second_result = read_tiff_placement(directory / "mapping_0001.tif", maximum_dimension);
  if (!first_result.ok())
    return first_result.status();
  if (!second_result.ok())
    return second_result.status();
  first = *first_result;
  second = *second_result;
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
  auto seam_dimensions = read_png_dimensions(seam_path);
  cv::Mat seam;
  if (seam_dimensions.ok() && seam_dimensions->first == canvas->first && seam_dimensions->second == canvas->second) {
    try {
      seam = cv::imread(seam_path.string(), cv::IMREAD_GRAYSCALE);
    } catch (const cv::Exception&) {
      seam.release();
    } catch (const std::bad_alloc&) {
      return absl::ResourceExhaustedError("Unable to allocate decoded Hugin seam");
    }
  }
  double minimum = 0.0;
  double maximum = 0.0;
  if (!seam.empty())
    cv::minMaxLoc(seam, &minimum, &maximum);
  if (seam.empty() || seam.cols != canvas->first || seam.rows != canvas->second || maximum <= minimum) {
    try {
      status = create_hard_seam(seam_path, first, second, canvas->first, canvas->second);
    } catch (const cv::Exception& exception) {
      return absl::ResourceExhaustedError("Unable to safely generate Hugin seam: " + std::string(exception.what()));
    } catch (const std::bad_alloc&) {
      return absl::ResourceExhaustedError("Unable to allocate generated Hugin seam");
    }
    if (!status.ok())
      return status;
    auto generated_dimensions = read_png_dimensions(seam_path);
    if (!generated_dimensions.ok() || generated_dimensions->first != canvas->first ||
        generated_dimensions->second != canvas->second) {
      return absl::FailedPreconditionError("Generated Hugin seam has invalid dimensions");
    }
    seam = cv::imread(seam_path.string(), cv::IMREAD_GRAYSCALE);
    if (seam.empty())
      return absl::FailedPreconditionError("Generated Hugin seam is not decodable");
    cv::minMaxLoc(seam, &minimum, &maximum);
  }
  if (seam.type() != CV_8UC1 || seam.cols != canvas->first || seam.rows != canvas->second || maximum <= minimum)
    return absl::FailedPreconditionError("Hugin seam violates the decoded canvas/type contract");
  return fsync_path(seam_path);
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

std::string restrict_optimization_variables(const std::string& project) {
  std::istringstream input(project);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line))
    lines.push_back(line);

  const std::array<const char*, 3> variables = {"r1", "p1", "y1"};
  std::vector<std::string> updated;
  updated.reserve(lines.size() + variables.size() + 2);
  bool in_variable_block = false;
  bool replaced = false;
  auto append_variable_block = [&]() {
    for (const char* variable : variables)
      updated.emplace_back(std::string("v ") + variable);
    updated.emplace_back("v");
  };
  for (const std::string& current : lines) {
    if (current.rfind("# specify variables", 0) == 0) {
      updated.push_back(current);
      append_variable_block();
      in_variable_block = true;
      replaced = true;
      continue;
    }
    if (in_variable_block) {
      if (!current.empty() && current.front() == 'v')
        continue;
      in_variable_block = false;
    }
    updated.push_back(current);
  }
  if (!replaced) {
    const auto insertion = std::find_if(updated.begin(), updated.end(), [](const std::string& value) {
      return value.rfind("# control points", 0) == 0;
    });
    std::vector<std::string> block = {"# specify variables", "v r1", "v p1", "v y1", "v", ""};
    updated.insert(insertion, block.begin(), block.end());
  }

  std::ostringstream output;
  for (const std::string& value : updated)
    output << value << '\n';
  return output.str();
}

absl::Status run_autooptimiser(const std::string& autooptimiser, const fs::path& directory) {
  // Optimize only the explicitly selected second-camera roll/pitch/yaw
  // variables and level the result. In particular, do not use -s: its
  // heuristic projection choice can flip between cylindrical and equirectangular
  // after a single marginal control-point change.
  std::vector<std::string> command = {autooptimiser, "-n", "-l", "-q", "-o", "autooptimiser_out.pto", "hm_project.pto"};
  std::string output;
  auto status = run_checked(command, directory, &output);
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

absl::Status fit_cylindrical_canvas(const std::string& pano_modify, const fs::path& directory) {
  const std::string temporary = "autooptimiser_fitted.pto";
  auto status = run_checked(
      {pano_modify, "--projection=1", "--fov=AUTO", "--canvas=AUTO", "-o", temporary, "autooptimiser_out.pto"},
      directory);
  if (!status.ok())
    return status;
  status = validate_nonempty_file(directory / temporary);
  if (!status.ok())
    return status;
  std::error_code error;
  fs::rename(directory / temporary, directory / "autooptimiser_out.pto", error);
  if (error)
    return absl::InternalError("Unable to publish fitted cylindrical Hugin project: " + error.message());
  return absl::OkStatus();
}

absl::Status scale_canvas(const std::string& pano_modify, const fs::path& directory, size_t width, size_t height) {
  if (width == 0 || height == 0)
    return absl::InvalidArgumentError("Scaled Hugin canvas dimensions must be positive");
  const std::string temporary = "autooptimiser_scaled.pto";
  auto status = run_checked(
      {pano_modify,
       "--canvas=" + std::to_string(width) + "x" + std::to_string(height),
       "-o",
       temporary,
       "autooptimiser_out.pto"},
      directory);
  if (!status.ok())
    return status;
  status = validate_nonempty_file(directory / temporary);
  if (!status.ok())
    return status;
  std::error_code error;
  fs::rename(directory / temporary, directory / "autooptimiser_out.pto", error);
  if (error)
    return absl::InternalError("Unable to publish scaled Hugin project: " + error.message());
  return absl::OkStatus();
}

absl::Status run_nona(const std::string& nona, const fs::path& directory) {
  remove_mapping_outputs(directory);
  return run_checked(
      {nona, "-m", "TIFF_m", "-z", "NONE", "--bigtiff", "-c", "-o", "mapping_", "autooptimiser_out.pto"}, directory);
}

absl::Status publish_artifacts(const fs::path& staging, const fs::path& game_dir, bool* prepared) {
  const std::vector<std::string> names = artifact_names();
  const fs::path backups = staging / "previous";
  std::error_code error;
  fs::create_directory(backups, error);
  if (error)
    return absl::InternalError("Unable to prepare stitch artifact rollback directory: " + error.message());
  for (const std::string& name : names) {
    if (!fs::exists(game_dir / name, error)) {
      error.clear();
      continue;
    }
    auto status = copy_file_preserving_mtime(game_dir / name, backups / name);
    if (!status.ok())
      return absl::InternalError(
          "Unable to preserve previous stitch artifact " + name + ": " + std::string(status.message()));
  }

  std::ostringstream manifest;
  for (const std::string& name : names)
    manifest << name << '\n';
  auto status = write_transaction_file(staging / "artifacts", manifest.str());
  if (!status.ok())
    return status;
  status = fsync_path(backups, true);
  if (!status.ok())
    return status;
  status = fsync_path(staging, true);
  if (!status.ok())
    return status;
  status = write_transaction_file(staging / "state", "PREPARED\n");
  if (!status.ok())
    return status;
  status = fsync_path(staging, true);
  if (!status.ok())
    return status;
  // The PREPARED state is only recoverable after the staging directory entry
  // itself is durable in its parent. Do this before changing any flat
  // artifacts in the game directory.
  status = fsync_path(game_dir, true);
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
  for (const std::string& name : names) {
    fs::remove(game_dir / name, error);
    if (error)
      return rollback_error("Unable to remove old stitch artifact " + name + ": " + error.message());
  }
  for (const std::string& name : names) {
    if (!fs::exists(staging / name, error)) {
      error.clear();
      continue;
    }
    fs::rename(staging / name, game_dir / name, error);
    if (error)
      return rollback_error("Unable to publish stitch artifact " + name + ": " + error.message());
    status = fsync_path(game_dir / name);
    if (!status.ok())
      return rollback_error(std::string(status.message()));
  }
  status = fsync_path(game_dir, true);
  if (!status.ok())
    return rollback_error(std::string(status.message()));
  status = write_transaction_file(staging / "state.committed", "COMMITTED\n");
  if (!status.ok())
    return rollback_error(std::string(status.message()));
  fs::rename(staging / "state.committed", staging / "state", error);
  if (error)
    return rollback_error("Unable to commit stitch transaction: " + error.message());
  status = fsync_path(staging, true);
  if (!status.ok())
    return status;
  fs::remove_all(staging, error);
  if (error)
    return absl::InternalError("Unable to clean committed stitch transaction: " + error.message());
  status = fsync_path(game_dir, true);
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

absl::StatusOr<std::string> HuginProject::GenerationId(const fs::path& game_dir, const ArtifactLock&) {
  std::ostringstream generation;
  // The stitched surface is determined by the PTO/remap generation. Legacy
  // valid generations did not publish left.png/right.png, so do not make
  // those provenance images part of the runtime identity.
  for (const char* name : kLegacyRequiredArtifacts) {
    struct stat metadata{};
    const fs::path path = game_dir / name;
    if (::stat(path.c_str(), &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
      return absl::NotFoundError("Hugin generation artifact is missing: " + path.string());
    }
    generation << name << ':' << static_cast<uint64_t>(metadata.st_dev) << ':' << static_cast<uint64_t>(metadata.st_ino)
               << ':' << static_cast<uint64_t>(metadata.st_size) << ':' << metadata.st_mtim.tv_sec << ':'
               << metadata.st_mtim.tv_nsec << '\n';
  }
  return generation.str();
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
  return Configure(game_dir, game_dir / "left.png", game_dir / "right.png", matches, options);
}

absl::Status HuginProject::Configure(
    const fs::path& game_dir,
    const fs::path& left_image,
    const fs::path& right_image,
    const std::vector<FeatureMatch>& matches,
    const Options& options) {
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
  auto autooptimiser = executable("HM_AUTOOPTIMISER", "autooptimiser");
  if (!autooptimiser.ok())
    return autooptimiser.status();

  std::ostringstream fov;
  fov.imbue(std::locale::classic());
  fov << std::setprecision(12) << options.horizontal_fov;
  auto status =
      run_checked({*pto_gen, "-p", "1", "-o", "hm_project.pto", "-f", fov.str(), "left.png", "right.png"}, staging);
  if (!status.ok())
    return status;
  auto project = read_file(staging / "hm_project.pto");
  if (!project.ok())
    return project.status();
  auto with_points = InsertControlPoints(*project, matches);
  if (!with_points.ok())
    return with_points.status();
  status = write_file(staging / "hm_project.pto", restrict_optimization_variables(*with_points));
  if (!status.ok())
    return status;

  status = run_autooptimiser(*autooptimiser, staging);
  if (!status.ok())
    return status;
  if (options.progress)
    options.progress("optimizer", "complete", "Panorama alignment optimized");
  if (options.progress)
    options.progress("canvas", "started", "Building stitch maps and panorama preview");
  auto nona = executable("HM_NONA", "nona");
  if (!nona.ok())
    return nona.status();
  auto optimized_project = read_file(staging / "autooptimiser_out.pto");
  if (!optimized_project.ok())
    return optimized_project.status();
  status = write_file(staging / "autooptimiser_out.pto", restrict_optimization_variables(*optimized_project));
  if (!status.ok())
    return status;
  auto pano_modify_result = executable("HM_PANO_MODIFY", "pano_modify");
  if (!pano_modify_result.ok())
    return pano_modify_result.status();
  const std::string pano_modify = *pano_modify_result;
  status = fit_cylindrical_canvas(pano_modify, staging);
  if (!status.ok())
    return status;
  auto fitted_project = read_file(staging / "autooptimiser_out.pto");
  if (!fitted_project.ok())
    return fitted_project.status();
  auto projection = ParseProjection(*fitted_project);
  if (!projection.ok() || *projection != 1)
    return absl::FailedPreconditionError("Hugin failed to preserve the required cylindrical projection");
  auto fit_canvas = [&](size_t width, size_t height, double rounding_guard) -> absl::Status {
    const size_t longest = std::max(width, height);
    const double factor =
        static_cast<double>(*options.max_canvas_dimension) / static_cast<double>(longest) * rounding_guard;
    const size_t scaled_width = std::max<size_t>(1, static_cast<size_t>(std::floor(width * factor)));
    const size_t scaled_height = std::max<size_t>(1, static_cast<size_t>(std::floor(height * factor)));
    return scale_canvas(pano_modify, staging, scaled_width, scaled_height);
  };
  if (options.max_canvas_dimension.has_value()) {
    auto optimized = read_file(staging / "autooptimiser_out.pto");
    if (!optimized.ok())
      return optimized.status();
    auto dimensions = ParseCanvasSize(*optimized);
    if (!dimensions.ok())
      return dimensions.status();
    const size_t longest = std::max(dimensions->first, dimensions->second);
    if (longest > *options.max_canvas_dimension) {
      status = fit_canvas(dimensions->first, dimensions->second, 1.0);
      if (!status.ok())
        return status;
    }
  }

  for (int attempt = 0; attempt < 3; ++attempt) {
    status = run_nona(*nona, staging);
    if (!status.ok())
      return status;
    bool mappings_valid = true;
    for (size_t index = 2; index < kRequiredArtifacts.size(); ++index) {
      status = validate_nonempty_file(staging / kRequiredArtifacts[index]);
      if (!status.ok()) {
        mappings_valid = false;
        break;
      }
    }
    if (!mappings_valid)
      return status;
    if (!options.max_canvas_dimension.has_value())
      break;

    // Nona derives its mapping canvas from the optimized PTO. Validate that
    // exact final contract and retry with a small rounding guard if necessary.
    auto optimized = read_file(staging / "autooptimiser_out.pto");
    if (!optimized.ok())
      return optimized.status();
    auto dimensions = ParseCanvasSize(*optimized);
    if (!dimensions.ok())
      return dimensions.status();
    const size_t longest = std::max(dimensions->first, dimensions->second);
    if (longest <= *options.max_canvas_dimension)
      break;
    if (attempt == 2) {
      return absl::FailedPreconditionError("Hugin mapping canvas still exceeds maximum dimension after three attempts");
    }
    status = fit_canvas(dimensions->first, dimensions->second, 0.999);
    if (!status.ok())
      return status;
  }

  // Enblend's preview/seam is preferred. The complete decoded artifact
  // validator below creates a hard-seam fallback inside this transaction.
  auto enblend = executable("HM_ENBLEND", "enblend");
  if (enblend.ok()) {
    status = run_checked(
        {*enblend, "--save-masks=seam_file.png", "-o", "panorama.tif", "mapping_0000.tif", "mapping_0001.tif"},
        staging);
    if (!status.ok()) {
      std::cerr << "Warning: native enblend preview failed; a hard seam will be generated: " << status << '\n';
      fs::remove(staging / "seam_file.png", error);
      error.clear();
      fs::remove(staging / "panorama.tif", error);
      error.clear();
    }
  }

  status = validate_staged_artifacts(staging, options.max_canvas_dimension);
  if (!status.ok())
    return status;
  status = publish_artifacts(staging, game_dir, &cleanup.prepared);
  if (status.ok() && options.progress)
    options.progress("canvas", "complete", "Stitch maps and panorama preview are ready");
  return status;
}

} // namespace hm::stitching
