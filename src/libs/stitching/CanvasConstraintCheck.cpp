#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
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

bool has_interrupted_transaction(const fs::path& game_dir) {
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(game_dir, error)) {
    if (error)
      return true;
    if (entry.path().filename().string().rfind(kStitchTransactionPrefix, 0) == 0)
      return true;
  }
  return error.operator bool();
}

} // namespace

CanvasConstraintArtifactLock::~CanvasConstraintArtifactLock() {
  if (descriptor_ >= 0) {
    ::flock(descriptor_, LOCK_UN);
    ::close(descriptor_);
  }
}

absl::StatusOr<LightweightCanvasConstraintCheck> try_lock_canvas_constraint_check(
    const fs::path& game_dir,
    size_t max_output_width) {
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
      return LightweightCanvasConstraintCheck{
          .artifact_lock = nullptr,
          .artifacts_compatible = false,
          .requires_regeneration = true,
      };
    }
    return absl::InternalError("Unable to lock stitching artifacts: " + std::string(std::strerror(lock_error)));
  }
  auto lock = std::unique_ptr<CanvasConstraintArtifactLock>(new CanvasConstraintArtifactLock(descriptor));
  auto compatibility = check_canvas_constraint_locked(game_dir, max_output_width);
  if (!compatibility.ok())
    return compatibility.status();
  return LightweightCanvasConstraintCheck{
      .artifact_lock = std::move(lock),
      .artifacts_compatible = compatibility->artifacts_compatible,
      .requires_regeneration = compatibility->requires_regeneration,
  };
}

absl::StatusOr<CanvasConstraintCompatibility> check_canvas_constraint_locked(
    const fs::path& game_dir,
    size_t max_output_width) {
  const bool has_mappings = any_mapping_artifact_exists(game_dir);
  if (has_interrupted_transaction(game_dir))
    return CanvasConstraintCompatibility{.requires_regeneration = has_mappings};
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
  return CanvasConstraintCompatibility{
      .artifacts_compatible = compatible,
      .requires_regeneration = !compatible,
  };
}

} // namespace hm::stitching
