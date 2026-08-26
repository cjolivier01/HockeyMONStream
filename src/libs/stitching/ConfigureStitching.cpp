#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/CalibrationModels.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/FeatureMatcher.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/Orientation.h"
#include "hstream/src/libs/stitching/RinkSegmentation.h"
#include "hstream/src/libs/stitching/ScoreboardSelector.h"
#include "hstream/src/libs/stitching/Synchronization.h"
#include "hstream/src/libs/stitching/TransactionState.h"

#include <yaml-cpp/yaml.h>

#include "cupano/pano/cudaMat.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <opencv2/opencv.hpp>
#include <sys/file.h>
#include <sys/stat.h>
#include <tiffio.h>
#include <unistd.h>
#include "absl/strings/str_split.h"

namespace fs = std::filesystem;

namespace hm {
namespace stitching {

namespace {

constexpr size_t kDefaultJetsonMaxLiveStitchCanvasDimension = 8192;
constexpr size_t kDefaultMaxControlPoints = 1500;
constexpr size_t kHardMaximumArtifactDimension = 32768;
constexpr uint64_t kHardMaximumArtifactPixels = 128ULL * 1024ULL * 1024ULL;

void report_calibration_progress(const std::string& stage, const std::string& status, const std::string& message = {}) {
  std::cout << "HSTREAM_CALIBRATION stage=" << stage << " status=" << status;
  if (!message.empty())
    std::cout << " message=" << message;
  std::cout << std::endl;
}

absl::Status recover_rink_transactions_locked(const fs::path& root);
absl::Status fsync_path(const fs::path& path, bool directory = false);

absl::Status link_rink_rollback_file(const fs::path& source, const fs::path& destination) {
  if (::link(source.c_str(), destination.c_str()) != 0) {
    return absl::InternalError("Unable to hard-link rink artifact for rollback: " + std::string(std::strerror(errno)));
  }
  return absl::OkStatus();
}

absl::StatusOr<size_t> remove_file_if_present(const fs::path& path) {
  std::error_code ec;
  const bool removed = fs::remove(path, ec);
  if (ec) {
    return absl::InternalError(TO_STRING("Failed to delete file \"" << path.string() << "\": " << ec.message()));
  }
  return removed ? 1 : 0;
}

std::string to_regex_pattern(const std::string& wildcard_pattern) {
  std::string out;
  out.reserve(wildcard_pattern.size() + 8);
  out.push_back('^');
  for (unsigned char c : wildcard_pattern) {
    switch (c) {
      case '*':
        out += ".*";
        break;
      case '.':
      case '^':
      case '$':
      case '+':
      case '?':
      case '(':
      case ')':
      case '[':
      case ']':
      case '{':
      case '}':
      case '|':
      case '\\':
        out.push_back('\\');
        out.push_back(c);
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  out.push_back('$');
  return out;
}

absl::StatusOr<size_t> clean_files_matching(const fs::path& game_dir, const std::string& pattern) {
  size_t removed = 0;
  if (pattern.find('*') == std::string::npos) {
    size_t direct_removed = 0;
    HM_ASSIGN_OR_RETURN(direct_removed, remove_file_if_present(game_dir / pattern));
    removed += direct_removed;
    return removed;
  }

  std::regex rgx(to_regex_pattern(pattern));
  std::error_code ec;
  if (!fs::exists(game_dir, ec) || !fs::is_directory(game_dir, ec) || ec) {
    if (ec) {
      return absl::InternalError(
          TO_STRING("Failed to inspect game directory \"" << game_dir.string() << "\": " << ec.message()));
    }
    return 0;
  }

  for (auto it = fs::directory_iterator(game_dir, ec); it != fs::directory_iterator(); it.increment(ec)) {
    if (ec) {
      return absl::InternalError(
          TO_STRING("Failed to iterate stitch game directory \"" << game_dir.string() << "\": " << ec.message()));
    }
    if (!it->is_regular_file(ec)) {
      if (ec) {
        return absl::InternalError(
            TO_STRING("Failed to query file type for \"" << it->path().string() << "\": " << ec.message()));
      }
      continue;
    }
    if (!std::regex_match(it->path().filename().string(), rgx)) {
      continue;
    }
    size_t entry_removed = 0;
    HM_ASSIGN_OR_RETURN(entry_removed, remove_file_if_present(it->path()));
    removed += entry_removed;
  }
  if (ec) {
    return absl::InternalError(
        TO_STRING("Failed to iterate stitch game directory \"" << game_dir.string() << "\": " << ec.message()));
  }
  return removed;
}

absl::StatusOr<size_t> delete_extracted_frames(const fs::path& game_dir) {
  static const std::set<std::string> kVideoExtensions = {".mp4", ".mkv", ".m4v", ".mov", ".avi"};
  size_t removed = 0;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(game_dir, ec); it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (ec) {
      return absl::InternalError(
          TO_STRING("Failed to iterate stitch game directory \"" << game_dir.string() << "\": " << ec.message()));
    }
    const fs::path p = it->path();
    if (!it->is_regular_file(ec)) {
      if (ec) {
        ec.clear();
      }
      continue;
    }
    std::string ext = p.extension().string();
    std::transform(
        ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (kVideoExtensions.find(ext) == kVideoExtensions.end()) {
      continue;
    }
    fs::path png = p;
    png.replace_extension(".png");
    size_t png_removed = 0;
    HM_ASSIGN_OR_RETURN(png_removed, remove_file_if_present(png));
    removed += png_removed;
  }
  if (ec) {
    return absl::InternalError(
        TO_STRING("Failed to iterate stitch game directory \"" << game_dir.string() << "\": " << ec.message()));
  }
  return removed;
}

std::optional<YAML::Node> map_child(const YAML::Node& node, const std::string& key) {
  if (!node.IsMap()) {
    return std::nullopt;
  }
  for (const auto& item : node) {
    if (item.first.as<std::string>() == key) {
      return item.second;
    }
  }
  return std::nullopt;
}

bool remove_yaml_key_path(YAML::Node& root, const std::initializer_list<std::string>& path) {
  if (!root.IsMap() || path.size() == 0) {
    return false;
  }
  std::vector<std::string> keys(path.begin(), path.end());
  YAML::Node current = root;
  std::vector<std::pair<YAML::Node, std::string>> path_nodes;
  for (size_t i = 0, n = keys.size(); i + 1 < n; ++i) {
    if (!current.IsMap()) {
      return false;
    }
    std::optional<YAML::Node> next = map_child(current, keys.at(i));
    if (!next.has_value() || !next->IsDefined()) {
      return false;
    }
    path_nodes.emplace_back(current, keys.at(i));
    current.reset(*next);
  }
  if (!current.IsMap()) {
    return false;
  }
  const std::string& leaf_key = keys.back();
  std::optional<YAML::Node> leaf = map_child(current, leaf_key);
  if (!leaf.has_value() || !leaf->IsDefined()) {
    return false;
  }
  current.remove(leaf_key);
  for (auto it = path_nodes.rbegin(); it != path_nodes.rend(); ++it) {
    YAML::Node parent = it->first;
    const std::string& key = it->second;
    YAML::Node child = parent[key];
    if (!child.IsMap() || child.size() != 0) {
      break;
    }
    parent.remove(key);
  }
  return true;
}

void remove_control_point_dependent_cache_keys(YAML::Node& config) {
  remove_yaml_key_path(config, {"stitching", "control_points"});
  remove_yaml_key_path(config, {"game", "stitching", "control_points"});
  remove_yaml_key_path(config, {"rink", "scoreboard", "perspective_polygon"});
  remove_yaml_key_path(config, {"rink", "ice_contours_mask_count"});
  remove_yaml_key_path(config, {"rink", "ice_contours_mask_centroid"});
  remove_yaml_key_path(config, {"rink", "ice_contours_combined_bbox"});
  remove_yaml_key_path(config, {"rink", "stitched_output_generation"});
  remove_yaml_key_path(config, {"rink", "stitched_output_persisted_rotation_degrees"});
  remove_yaml_key_path(config, {"rink", "stitched_output_pending_generation"});
  remove_yaml_key_path(config, {"rink", "stitched_output_pending_authorization_id"});
  remove_yaml_key_path(config, {"rink", "stitched_output_pending_previous_generation"});
  remove_yaml_key_path(config, {"rink", "stitched_output_pending_previous_authorization_id"});
  remove_yaml_key_path(config, {"rink", "stitched_output_pending_completed_scoreboard_polygon"});
}

void remove_cleanable_stitching_cache_keys(YAML::Node& config) {
  remove_yaml_key_path(config, {"stitching", "frame_offsets"});
  remove_yaml_key_path(config, {"game", "stitching", "frame_offsets"});
  remove_control_point_dependent_cache_keys(config);
}

bool is_empty_yaml_document(const YAML::Node& node) {
  return !node.IsDefined() || node.IsNull() || ((node.IsMap() || node.IsSequence()) && node.size() == 0);
}

struct TiffPlacement {
  float x_px{0.0f};
  float y_px{0.0f};
  int width{0};
  int height{0};
};

struct CanvasSize {
  size_t width{0};
  size_t height{0};
};

struct ScaledCanvas {
  CanvasSize size;
  double scale{1.0};
};

std::optional<size_t> get_positive_env_size(const char* name) {
  const char* value = std::getenv(name);
  if (!value || !*value) {
    return std::nullopt;
  }
  try {
    const unsigned long long parsed = std::stoull(value);
    if (parsed > 0) {
      return static_cast<size_t>(parsed);
    }
  } catch (const std::exception&) {
  }
  std::cerr << "Warning: ignoring invalid " << name << "=" << value << std::endl;
  return std::nullopt;
}

std::optional<size_t> live_stitch_max_canvas_dimension() {
  const char* allow_oversized = std::getenv("HM_ALLOW_OVERSIZED_LIVE_STITCH");
  if (allow_oversized && std::string(allow_oversized) == "1") {
    return std::nullopt;
  }
  if (auto from_env = get_positive_env_size("HM_MAX_LIVE_STITCH_EGL_DIMENSION"); from_env.has_value()) {
    return from_env;
  }
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  return kDefaultJetsonMaxLiveStitchCanvasDimension;
#else
  return std::nullopt;
#endif
}

bool canvas_exceeds_max_dimension(const CanvasSize& canvas, size_t max_dimension) {
  return canvas.width > max_dimension || canvas.height > max_dimension;
}

absl::StatusOr<size_t> parse_exact_size_t(const std::string& value, const char* field_name) {
  if (value.empty())
    return absl::InvalidArgumentError(TO_STRING("Invalid stitched-output " << field_name));
  size_t parsed = 0;
  size_t consumed = 0;
  try {
    parsed = std::stoull(value, &consumed);
  } catch (const std::exception&) {
    return absl::InvalidArgumentError(TO_STRING("Invalid stitched-output " << field_name));
  }
  if (consumed != value.size() || parsed == 0)
    return absl::InvalidArgumentError(TO_STRING("Invalid stitched-output " << field_name));
  return parsed;
}

absl::StatusOr<TiffPlacement> read_tiff_placement(const fs::path& path) {
  TIFF* tif = TIFFOpen(path.c_str(), "r");
  if (!tif) {
    return absl::NotFoundError(TO_STRING("Could not open TIFF: " << path.string()));
  }

  uint32_t width = 0;
  uint32_t height = 0;
  float xres = 0.0f;
  float yres = 0.0f;
  float xpos = 0.0f;
  float ypos = 0.0f;

  const bool have_dims =
      TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width) && TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
  const bool have_res = TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres) && TIFFGetField(tif, TIFFTAG_YRESOLUTION, &yres);
  const bool have_position = TIFFGetField(tif, TIFFTAG_XPOSITION, &xpos) && TIFFGetField(tif, TIFFTAG_YPOSITION, &ypos);

  TIFFClose(tif);

  if (!have_dims || !width || !height) {
    return absl::InvalidArgumentError(TO_STRING("Missing TIFF dimensions: " << path.string()));
  }
  if (width > kHardMaximumArtifactDimension || height > kHardMaximumArtifactDimension ||
      static_cast<uint64_t>(width) * height > kHardMaximumArtifactPixels) {
    return absl::ResourceExhaustedError(TO_STRING("TIFF dimensions exceed safety limits: " << path.string()));
  }
  if (!have_res || !std::isfinite(xres) || !std::isfinite(yres) || xres <= 0.0f || yres <= 0.0f) {
    return absl::InvalidArgumentError(TO_STRING("Missing TIFF resolution: " << path.string()));
  }
  if (!have_position) {
    return absl::InvalidArgumentError(TO_STRING("Missing TIFF placement: " << path.string()));
  }

  const float x_px = xpos * xres;
  const float y_px = ypos * yres;
  if (!std::isfinite(xpos) || !std::isfinite(ypos) || !std::isfinite(x_px) || !std::isfinite(y_px) ||
      x_px < std::numeric_limits<int>::lowest() || x_px > std::numeric_limits<int>::max() ||
      y_px < std::numeric_limits<int>::lowest() || y_px > std::numeric_limits<int>::max()) {
    return absl::InvalidArgumentError(TO_STRING("Invalid TIFF placement: " << path.string()));
  }

  return TiffPlacement{
      .x_px = static_cast<float>(x_px),
      .y_px = static_cast<float>(y_px),
      .width = static_cast<int>(width),
      .height = static_cast<int>(height),
  };
}

absl::StatusOr<CanvasSize> normalize_and_measure_canvas(TiffPlacement* p0, TiffPlacement* p1) {
  const float min_x = std::min(p0->x_px, p1->x_px);
  const float min_y = std::min(p0->y_px, p1->y_px);
  p0->x_px -= min_x;
  p1->x_px -= min_x;
  p0->y_px -= min_y;
  p1->y_px -= min_y;

  const float width = std::max(p0->x_px + p0->width, p1->x_px + p1->width);
  const float height = std::max(p0->y_px + p0->height, p1->y_px + p1->height);
  if (!std::isfinite(width) || !std::isfinite(height) || width < 1.0 || height < 1.0 ||
      width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max()) {
    return absl::FailedPreconditionError("Mapping TIFFs produce an invalid canvas");
  }
  const auto canvas_width = static_cast<size_t>(width);
  const auto canvas_height = static_cast<size_t>(height);
  if (canvas_width > kHardMaximumArtifactDimension || canvas_height > kHardMaximumArtifactDimension ||
      static_cast<uint64_t>(canvas_width) * canvas_height > kHardMaximumArtifactPixels) {
    return absl::ResourceExhaustedError("Mapping TIFF canvas exceeds safety limits");
  }
  return CanvasSize{.width = canvas_width, .height = canvas_height};
}

ScaledCanvas scale_canvas_to_max_output_width(
    const TiffPlacement& p0,
    const TiffPlacement& p1,
    const CanvasSize& native,
    size_t max_output_width) {
  if (max_output_width == 0 || native.width <= max_output_width)
    return ScaledCanvas{.size = native, .scale = 1.0};
  auto size_for_scale = [&](double scale) {
    auto scaled_extent = [scale](const TiffPlacement& placement) {
      const auto x = static_cast<int>(std::floor(placement.x_px * scale));
      const auto y = static_cast<int>(std::floor(placement.y_px * scale));
      const auto right = static_cast<int>(std::ceil((placement.x_px + placement.width) * scale));
      const auto bottom = static_cast<int>(std::ceil((placement.y_px + placement.height) * scale));
      return std::tuple<int, int>{std::max(1, right - x) + x, std::max(1, bottom - y) + y};
    };
    auto [right0, bottom0] = scaled_extent(p0);
    auto [right1, bottom1] = scaled_extent(p1);
    return CanvasSize{
        .width = static_cast<size_t>(std::max(right0, right1)),
        .height = static_cast<size_t>(std::max(bottom0, bottom1)),
    };
  };
  const double direct_scale = static_cast<double>(max_output_width) / static_cast<double>(native.width);
  CanvasSize direct_size = size_for_scale(direct_scale);
  if (direct_size.width <= max_output_width)
    return ScaledCanvas{.size = direct_size, .scale = direct_scale};
  double low = 0.0;
  double high = direct_scale;
  for (int iteration = 0; iteration < 32; ++iteration) {
    const double mid = (low + high) / 2.0;
    if (size_for_scale(mid).width <= max_output_width) {
      low = mid;
    } else {
      high = mid;
    }
  }
  const double selected_scale = low > 0.0 ? low : high;
  return ScaledCanvas{.size = size_for_scale(selected_scale), .scale = selected_scale};
}

absl::StatusOr<CanvasSize> get_mapping_canvas_size(const fs::path& game_dir) {
  const fs::path mapping0_path = game_dir / "mapping_0000.tif";
  const fs::path mapping1_path = game_dir / "mapping_0001.tif";
  if (!fs::exists(mapping0_path) || !fs::exists(mapping1_path)) {
    return absl::NotFoundError(
        TO_STRING("Cannot determine stitched canvas size; missing mapping TIFFs under " << game_dir.string()));
  }

  TiffPlacement p0;
  TiffPlacement p1;
  HM_ASSIGN_OR_RETURN(p0, read_tiff_placement(mapping0_path));
  HM_ASSIGN_OR_RETURN(p1, read_tiff_placement(mapping1_path));

  return normalize_and_measure_canvas(&p0, &p1);
}

absl::StatusOr<CanvasSize> get_effective_mapping_canvas_size(const fs::path& game_dir, size_t max_output_width) {
  TiffPlacement p0;
  TiffPlacement p1;
  HM_ASSIGN_OR_RETURN(p0, read_tiff_placement(game_dir / "mapping_0000.tif"));
  HM_ASSIGN_OR_RETURN(p1, read_tiff_placement(game_dir / "mapping_0001.tif"));
  CanvasSize native;
  HM_ASSIGN_OR_RETURN(native, normalize_and_measure_canvas(&p0, &p1));
  return scale_canvas_to_max_output_width(p0, p1, native, max_output_width).size;
}

absl::StatusOr<CanvasSize> read_remap_tiff_header(const fs::path& path) {
  TIFF* tif = TIFFOpen(path.c_str(), "r");
  if (!tif) {
    return absl::NotFoundError(TO_STRING("Could not open remap TIFF: " << path.string()));
  }
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t samples = 0;
  uint16_t bits = 0;
  uint16_t sample_format = SAMPLEFORMAT_UINT;
  const bool have_dims =
      TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width) && TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
  TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sample_format);
  TIFFClose(tif);
  if (!have_dims || !width || !height || width > kHardMaximumArtifactDimension ||
      height > kHardMaximumArtifactDimension || static_cast<uint64_t>(width) * height > kHardMaximumArtifactPixels ||
      samples != 1 || bits != 16 || sample_format != SAMPLEFORMAT_UINT) {
    return absl::InvalidArgumentError(TO_STRING("Invalid remap TIFF metadata: " << path.string()));
  }
  return CanvasSize{.width = static_cast<size_t>(width), .height = static_cast<size_t>(height)};
}

absl::Status validate_remap_artifact_headers(
    const fs::path& game_dir,
    const TiffPlacement& left_placement,
    const TiffPlacement& right_placement) {
  CanvasSize left_x;
  CanvasSize left_y;
  CanvasSize right_x;
  CanvasSize right_y;
  HM_ASSIGN_OR_RETURN(left_x, read_remap_tiff_header(game_dir / "mapping_0000_x.tif"));
  HM_ASSIGN_OR_RETURN(left_y, read_remap_tiff_header(game_dir / "mapping_0000_y.tif"));
  HM_ASSIGN_OR_RETURN(right_x, read_remap_tiff_header(game_dir / "mapping_0001_x.tif"));
  HM_ASSIGN_OR_RETURN(right_y, read_remap_tiff_header(game_dir / "mapping_0001_y.tif"));
  if (left_x.width != left_y.width || left_x.height != left_y.height || right_x.width != right_y.width ||
      right_x.height != right_y.height) {
    return absl::FailedPreconditionError("Stitching remap X/Y dimensions do not match");
  }
  if (left_x.width != static_cast<size_t>(left_placement.width) ||
      left_x.height != static_cast<size_t>(left_placement.height) ||
      right_x.width != static_cast<size_t>(right_placement.width) ||
      right_x.height != static_cast<size_t>(right_placement.height)) {
    return absl::FailedPreconditionError("Stitching placement and remap dimensions do not match");
  }
  return absl::OkStatus();
}

bool artifacts_exceed_live_canvas_limit(
    const CanvasSize& native_canvas,
    const CanvasSize& effective_canvas,
    size_t max_dimension) {
  return canvas_exceeds_max_dimension(native_canvas, max_dimension) &&
      canvas_exceeds_max_dimension(effective_canvas, max_dimension);
}

struct CanvasProvenanceCompatibility {
  bool compatible{false};
  std::string reason;
};

long double constrained_canvas_scale(
    const HuginProject::CanvasProvenance& provenance,
    size_t max_output_width,
    size_t max_canvas_dimension) {
  long double scale = 1.0L;
  if (max_output_width > 0 && provenance.source_canvas_width > max_output_width) {
    scale = std::min(
        scale, static_cast<long double>(max_output_width) / static_cast<long double>(provenance.source_canvas_width));
  }
  const size_t source_longest = std::max(provenance.source_canvas_width, provenance.source_canvas_height);
  if (max_canvas_dimension > 0 && source_longest > max_canvas_dimension) {
    scale = std::min(scale, static_cast<long double>(max_canvas_dimension) / static_cast<long double>(source_longest));
  }
  return scale;
}

CanvasProvenanceCompatibility check_canvas_provenance_compatibility(
    const std::optional<HuginProject::CanvasProvenance>& provenance,
    const CanvasSize& published_canvas,
    size_t max_output_width,
    const std::optional<size_t>& max_canvas_dimension) {
  if (!provenance.has_value()) {
    return {false, "canvas provenance is missing; legacy capped artifacts cannot be identified safely"};
  }
  if (provenance->canvas_width != published_canvas.width || provenance->canvas_height != published_canvas.height) {
    return {false, "published canvas dimensions do not match canvas provenance"};
  }
  if (max_output_width > 0 && published_canvas.width > max_output_width) {
    return {false, "the published canvas exceeds the current maximum output width"};
  }

  const size_t current_max_canvas_dimension = max_canvas_dimension.value_or(0);
  if (current_max_canvas_dimension > 0 &&
      canvas_exceeds_max_dimension(published_canvas, current_max_canvas_dimension)) {
    return {false, "the published canvas exceeds the current live canvas limit"};
  }
  if (provenance->max_output_width_applied || provenance->max_canvas_dimension_applied) {
    const long double generated_scale =
        constrained_canvas_scale(*provenance, provenance->max_output_width, provenance->max_canvas_dimension);
    const long double current_scale =
        constrained_canvas_scale(*provenance, max_output_width, current_max_canvas_dimension);
    constexpr long double kScaleTolerance = 1e-15L;
    if (std::abs(generated_scale - current_scale) > kScaleTolerance) {
      return {false, "the combined effective canvas scale changed"};
    }
  }
  return {true, {}};
}

// -----------------------------------------------------------------------------
// FileNode: Represents one file and its dependency children.
// A file that depends on multiple parents can simply be listed as a child
// under each parent.
// -----------------------------------------------------------------------------
struct FileNode {
  std::string filename;
  std::vector<FileNode> children;
};

// -----------------------------------------------------------------------------
// ValidationResult: Returned by the checker function.
//   - valid: true if every child is newer than its parent for every dependency edge.
//   - levels: a list of tree depths (starting at 0 for the root) where at least one violation occurred.
// -----------------------------------------------------------------------------
struct TimestampedFile {
  std::string filename;
  fs::file_time_type time;
};

struct DependencyReportNode {
  std::string filename;
  int level = 0;
  bool direct_invalid = false;
  bool invalidated_by_upstream = false;
  std::vector<std::string> reasons;
  std::vector<DependencyReportNode> children;

  bool has_invalid_subtree() const {
    if (direct_invalid || invalidated_by_upstream) {
      return true;
    }
    return std::any_of(children.begin(), children.end(), [](const DependencyReportNode& child) {
      return child.has_invalid_subtree();
    });
  }
};

struct ValidationResult {
  bool valid = true;
  std::vector<int> levels;
  DependencyReportNode report;
};

std::vector<std::string> resolved_dependency_filenames(const fs::path& dir_name, const std::string& filename) {
  std::vector<std::string> filenames = absl::StrSplit(filename, ',');
  if (!dir_name.empty()) {
    for (auto& s : filenames) {
      s = (dir_name / s).string();
    }
  }
  return filenames;
}

std::string time_for_log(fs::file_time_type time) {
  return std::to_string(time.time_since_epoch().count());
}

std::string quote(const std::string& s) {
  return "\"" + s + "\"";
}

std::vector<std::string> missing_filenames(const std::vector<std::string>& filenames) {
  std::vector<std::string> missing;
  for (const auto& filename : filenames) {
    std::error_code ec;
    if (!fs::exists(filename, ec) || ec) {
      missing.push_back(filename);
    }
  }
  return missing;
}

std::optional<TimestampedFile> most_recent_write_time(const std::vector<std::string>& filenames) {
  std::optional<TimestampedFile> latest;
  for (const auto& filename : filenames) {
    std::error_code ec;
    const fs::file_time_type ftime = fs::last_write_time(filename, ec);
    if (ec) {
      std::cerr << "Error retrieving last write time for " << filename << ": " << ec.message() << std::endl;
      continue;
    }
    if (!latest.has_value() || ftime > latest->time) {
      TimestampedFile timestamped_file;
      timestamped_file.filename = filename;
      timestamped_file.time = ftime;
      latest = timestamped_file;
    }
  }
  return latest;
}

std::optional<TimestampedFile> oldest_write_time(const std::vector<std::string>& filenames) {
  std::optional<TimestampedFile> oldest;
  for (const auto& filename : filenames) {
    std::error_code ec;
    const fs::file_time_type ftime = fs::last_write_time(filename, ec);
    if (ec) {
      std::cerr << "Error retrieving last write time for " << filename << ": " << ec.message() << std::endl;
      continue;
    }
    if (!oldest.has_value() || ftime < oldest->time) {
      TimestampedFile timestamped_file;
      timestamped_file.filename = filename;
      timestamped_file.time = ftime;
      oldest = timestamped_file;
    }
  }
  return oldest;
}

void collect_direct_violation_levels(const DependencyReportNode& node, std::vector<int>* levels) {
  if (node.direct_invalid) {
    levels->push_back(node.level);
  }
  for (const auto& child : node.children) {
    collect_direct_violation_levels(child, levels);
  }
}

void print_dependency_report_node(
    const DependencyReportNode& node,
    const std::string& prefix,
    bool is_last,
    std::ostream& out) {
  const std::string connector = is_last ? "`- " : "+- ";
  out << prefix << connector << node.filename << " [";
  if (node.direct_invalid) {
    out << "invalid";
    if (node.invalidated_by_upstream) {
      out << ", invalidated downstream";
    }
  } else {
    out << "invalidated downstream";
  }
  out << "]\n";

  const std::string child_prefix = prefix + (is_last ? "   " : "|  ");
  for (const auto& reason : node.reasons) {
    out << child_prefix << "reason: " << reason << "\n";
  }

  std::vector<const DependencyReportNode*> invalid_children;
  for (const auto& child : node.children) {
    if (child.has_invalid_subtree()) {
      invalid_children.push_back(&child);
    }
  }
  for (size_t i = 0; i < invalid_children.size(); ++i) {
    print_dependency_report_node(*invalid_children[i], child_prefix, i + 1 == invalid_children.size(), out);
  }
}

void print_dependency_report(const DependencyReportNode& root, std::ostream& out) {
  std::vector<const DependencyReportNode*> report_roots;
  if (root.direct_invalid || root.invalidated_by_upstream) {
    report_roots.push_back(&root);
  } else {
    for (const auto& child : root.children) {
      if (child.has_invalid_subtree()) {
        report_roots.push_back(&child);
      }
    }
  }

  if (report_roots.empty()) {
    return;
  }

  out << "Dependency invalidation tree:\n";
  for (size_t i = 0; i < report_roots.size(); ++i) {
    print_dependency_report_node(*report_roots[i], "", i + 1 == report_roots.size(), out);
  }
}

// -----------------------------------------------------------------------------
// checkFileDependencies:
// Recursively checks that for every dependency edge, the child file's modification time
// is strictly newer than its parent's modification time. The function accumulates the
// tree level (depth) at which any violation is found.
//
// Note: If a file appears multiple times (i.e. as a child of two different parents), it will be checked
// for each dependency. If a violation occurs in any dependency edge, that level is recorded.
// -----------------------------------------------------------------------------
DependencyReportNode build_dependency_report(
    const fs::path& dir_name,
    const FileNode& node,
    int level,
    const std::optional<TimestampedFile>& parent_latest,
    bool invalidated_by_upstream,
    const std::string& invalid_upstream_item) {
  DependencyReportNode report;
  report.filename = node.filename;
  report.level = level;
  report.invalidated_by_upstream = invalidated_by_upstream;

  if (invalidated_by_upstream) {
    report.reasons.push_back("depends on invalid upstream item " + quote(invalid_upstream_item));
  }

  const std::vector<std::string> filenames = resolved_dependency_filenames(dir_name, node.filename);
  const std::vector<std::string> missing = missing_filenames(filenames);
  for (const auto& filename : missing) {
    report.direct_invalid = true;
    report.reasons.push_back("missing file " + quote(filename));
  }

  std::optional<TimestampedFile> node_oldest;
  std::optional<TimestampedFile> node_latest;
  if (missing.empty()) {
    node_oldest = oldest_write_time(filenames);
    node_latest = most_recent_write_time(filenames);
    if (!node_oldest.has_value() || !node_latest.has_value()) {
      report.direct_invalid = true;
      report.reasons.push_back("could not read modification time for " + quote(node.filename));
    }
  }

  if (parent_latest.has_value() && node_oldest.has_value() && node_oldest->time < parent_latest->time) {
    report.direct_invalid = true;
    report.reasons.push_back(
        "oldest output " + quote(node_oldest->filename) + " (mtime=" + time_for_log(node_oldest->time) +
        ") is older than dependency " + quote(parent_latest->filename) +
        " (mtime=" + time_for_log(parent_latest->time) + ")");
  }

  const bool child_invalidated_by_upstream = invalidated_by_upstream || report.direct_invalid;
  const std::string child_invalid_upstream_item = report.direct_invalid ? node.filename : invalid_upstream_item;
  for (const auto& child : node.children) {
    report.children.push_back(build_dependency_report(
        dir_name, child, level + 1, node_latest, child_invalidated_by_upstream, child_invalid_upstream_item));
  }

  return report;
}

ValidationResult checkFileDependencies(const fs::path& dir_name, const FileNode& node, int level = 0) {
  ValidationResult result;
  result.report = build_dependency_report(
      dir_name,
      node,
      level,
      /*parent_latest=*/std::nullopt,
      /*invalidated_by_upstream=*/false,
      /*invalid_upstream_item=*/"");
  collect_direct_violation_levels(result.report, &result.levels);
  result.valid = result.levels.empty();
  return result;
}

// -----------------------------------------------------------------------------
// Example usage:
//
// Build a dependency tree such that a file might depend on multiple parents.
// For example, if grandchild1.txt depends on both child1.txt and child2.txt,
// then grandchild1.txt appears in both children vectors.
//
//   root.txt
//      ├── child1.txt
//      │       └── grandchild1.txt
//      └── child2.txt
//              └── grandchild1.txt
// -----------------------------------------------------------------------------

const char* level0 = "left.png,right.png";
const char* level1 = "hm_project.pto";
const char* level2 = "autooptimiser_out.pto";
const char* level3 =
    // Keep the dependency graph focused on the mapping generation so legacy/incomplete artifact sets can reach the
    // explicit seam validator and report how to regenerate the seam (or opt into the diagnostic hard-seam fallback).
    "mapping_0000.tif,mapping_0000_x.tif,mapping_0000_y.tif,mapping_0001.tif,mapping_0001_x.tif,mapping_0001_y.tif";
const char* level4 = "s.png";
const char* level5 = "rink_mask_0.png";

bool test_dependency_tree(const std::string& dir_name, bool add_rink_mask) {
  FileNode stitching_tree;
  stitching_tree.filename = level0;
  stitching_tree.children.emplace_back(FileNode{.filename = level1});
  stitching_tree.children[0].children.emplace_back(FileNode{.filename = level2});
  stitching_tree.children[0].children[0].children.emplace_back(FileNode{.filename = level3});
  if (add_rink_mask) {
    stitching_tree.children[0].children[0].children[0].children.emplace_back(FileNode{.filename = level4});
    stitching_tree.children[0].children[0].children[0].children[0].children.emplace_back(FileNode{.filename = level5});
  }

  // Perform the dependency check.
  ValidationResult res = checkFileDependencies(dir_name, stitching_tree);

  if (!res.valid) {
    // Optionally remove duplicates and sort.
    std::sort(res.levels.begin(), res.levels.end());
    res.levels.erase(std::unique(res.levels.begin(), res.levels.end()), res.levels.end());
    std::cout << "Dependency violations found at level(s): ";
    for (int lvl : res.levels) {
      std::cout << lvl << " ";
    }
    std::cout << "\n";
    print_dependency_report(res.report, std::cout);
    return false;
  }
  return true;
}

absl::StatusOr<cv::Mat> download_image(surface::Surface surf) {
  cv::Mat cpu_img;
  if (surf.get()->colorFormat == NVBUF_COLOR_FORMAT_RGBA) {
    CudaMat<uchar4> gpu_image(
        SurfaceInfo{
            .width = (int)surf.width(),
            .height = (int)surf.height(),
            .pitch = (int)surf.pitch(),
            .data_ptr = surf.dataptr(),
        },
        /*B=*/1);
    cpu_img = gpu_image.download();
  } else if (
      surf.get()->colorFormat == NVBUF_COLOR_FORMAT_RGBA_10_10_10_2_709 ||
      surf.get()->colorFormat == NVBUF_COLOR_FORMAT_RGBA_10_10_10_2_2020) {
    std::vector<uint32_t> packed(static_cast<size_t>(surf.width()) * surf.height());
    const cudaError_t copy_status = cudaMemcpy2D(
        packed.data(),
        static_cast<size_t>(surf.width()) * sizeof(uint32_t),
        surf.dataptr(),
        surf.pitch(),
        static_cast<size_t>(surf.width()) * sizeof(uint32_t),
        surf.height(),
        cudaMemcpyDeviceToHost);
    if (copy_status != cudaSuccess) {
      return absl::FailedPreconditionError(
          TO_STRING("Unable to download RGB10 calibration image from GPU: " << cudaGetErrorString(copy_status)));
    }
    cpu_img = cv::Mat(static_cast<int>(surf.height()), static_cast<int>(surf.width()), CV_16UC3);
    constexpr uint32_t kRgb10Mask = 0x3ffu;
    for (int y = 0; y < cpu_img.rows; ++y) {
      auto* row = cpu_img.ptr<cv::Vec3w>(y);
      const auto* packed_row = packed.data() + static_cast<size_t>(y) * surf.width();
      for (int x = 0; x < cpu_img.cols; ++x) {
        const uint32_t pixel = packed_row[x];
        const uint16_t red = static_cast<uint16_t>(((pixel & kRgb10Mask) * 65535u + 511u) / 1023u);
        const uint16_t green = static_cast<uint16_t>(((((pixel >> 10) & kRgb10Mask) * 65535u + 511u) / 1023u));
        const uint16_t blue = static_cast<uint16_t>(((((pixel >> 20) & kRgb10Mask) * 65535u + 511u) / 1023u));
        row[x] = cv::Vec3w(blue, green, red);
      }
    }
  } else {
    return absl::InvalidArgumentError("Invalid image format");
  }
  if (cpu_img.empty()) {
    return absl::FailedPreconditionError("Unable to download image from GPU");
  }
  return cpu_img;
}

absl::Status save_image(surface::Surface surf, const std::string& filename) {
  cv::Mat cpu_img;
  HM_ASSIGN_OR_RETURN(cpu_img, download_image(surf));
  if (!cv::imwrite(filename, cpu_img)) {
    return absl::FailedPreconditionError("Unable to write image to file");
  }
  return absl::OkStatus();
}

absl::StatusOr<cv::Mat> load_feature_image(const fs::path& image_file) {
  cv::Mat image = cv::imread(image_file.string(), cv::IMREAD_UNCHANGED);
  if (image.empty()) {
    return absl::FailedPreconditionError("Unable to reload synchronized frames for native feature matching");
  }
  if (image.channels() == 4) {
    cv::Mat bgr;
    if (image.depth() == CV_16U) {
      cv::cvtColor(image, bgr, cv::COLOR_BGRA2BGR);
    } else {
      cv::cvtColor(image, bgr, cv::COLOR_BGRA2BGR);
    }
    image = std::move(bgr);
  }
  if (image.channels() != 3 || (image.depth() != CV_8U && image.depth() != CV_16U)) {
    return absl::FailedPreconditionError("Reloaded feature image must be 8-bit or 16-bit BGR");
  }
  return image;
}

} // namespace

absl::Status save_stitched_image(
    const std::string& game_dir,
    surface::Surface surface,
    const std::string& producer_output_generation) {
  cv::Mat image;
  HM_ASSIGN_OR_RETURN(image, download_image(surface));
  return save_stitched_image(game_dir, image, producer_output_generation);
}

absl::Status save_stitched_image(
    const std::string& game_dir,
    const cv::Mat& image,
    const std::string& producer_output_generation) {
  if (game_dir.empty()) {
    return absl::InvalidArgumentError("Cannot save stitched scoreboard image without a game directory");
  }
  if (image.empty())
    return absl::InvalidArgumentError("Cannot save an empty stitched scoreboard image");
  if (producer_output_generation.empty()) {
    return absl::FailedPreconditionError(
        "Cannot publish a scoreboard snapshot without the stitched-output generation that produced it");
  }
  HM_RETURN_IF_ERROR(validate_stitched_output_generation_dimensions(
      producer_output_generation, static_cast<size_t>(image.cols), static_cast<size_t>(image.rows)));
  std::error_code ec;
  fs::create_directories(game_dir, ec);
  if (ec) {
    return absl::InternalError(TO_STRING("Failed to create game directory \"" << game_dir << "\": " << ec.message()));
  }

  const fs::path root(game_dir);
  std::string pattern = (root / ".s.png-XXXXXX.png").string();
  std::vector<char> writable_pattern(pattern.begin(), pattern.end());
  writable_pattern.push_back('\0');
  const int temporary_descriptor = ::mkstemps(writable_pattern.data(), 4);
  if (temporary_descriptor < 0) {
    return absl::InternalError(
        "Unable to create temporary stitched scoreboard image: " + std::string(std::strerror(errno)));
  }
  const fs::path temporary(writable_pattern.data());
  struct TemporaryCleanup {
    fs::path path;
    ~TemporaryCleanup() {
      if (path.empty())
        return;
      std::error_code ignored;
      fs::remove(path, ignored);
    }
  } cleanup{temporary};
  if (::close(temporary_descriptor) != 0) {
    return absl::InternalError(
        "Unable to close temporary stitched scoreboard image: " + std::string(std::strerror(errno)));
  }
  try {
    if (!cv::imwrite(temporary.string(), image))
      return absl::FailedPreconditionError("Unable to encode stitched scoreboard image");
  } catch (const cv::Exception& exception) {
    return absl::FailedPreconditionError(
        "Unable to encode stitched scoreboard image: " + std::string(exception.what()));
  } catch (const std::bad_alloc&) {
    return absl::ResourceExhaustedError("Unable to allocate stitched scoreboard image encoder state");
  }
  HM_RETURN_IF_ERROR(fsync_path(temporary));

  auto hugin_lock = HuginProject::RecoverAndLock(root);
  if (!hugin_lock.ok())
    return hugin_lock.status();
  auto config_lock = GameConfigTransactionLock::Acquire(root);
  if (!config_lock.ok())
    return config_lock.status();
  auto hugin_generation = HuginProject::GenerationId(root, **hugin_lock);
  if (!hugin_generation.ok())
    return hugin_generation.status();
  YAML::Node config(YAML::NodeType::Map);
  try {
    if (fs::is_regular_file(root / "config.yaml"))
      config = YAML::LoadFile((root / "config.yaml").string());
    const YAML::Node configured_generation = config["rink"]["stitched_output_generation"];
    if (!configured_generation || !configured_generation.IsScalar()) {
      return absl::FailedPreconditionError(
          "Cannot publish a scoreboard snapshot without a current stitched-output generation");
    }
    std::string current_generation;
    HM_ASSIGN_OR_RETURN(
        current_generation, current_stitched_output_generation_id_locked(game_dir, config, *hugin_generation));
    if (configured_generation.as<std::string>() != current_generation) {
      return absl::AbortedError("Cannot publish a scoreboard snapshot for stale stitched-output geometry");
    }
    if (producer_output_generation != current_generation) {
      return absl::AbortedError(
          "Cannot publish a scoreboard snapshot from a frame produced by a superseded stitched-output generation");
    }
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to validate scoreboard snapshot generation: " + std::string(exception.what()));
  }
  fs::rename(temporary, root / "s.png", ec);
  if (ec)
    return absl::InternalError("Unable to atomically publish stitched scoreboard image: " + ec.message());
  cleanup.path.clear();
  return fsync_path(root, true);
}

namespace {

absl::StatusOr<bool> read_stitching_invalidation_cleanup_state(
    const fs::path& config_path,
    const std::string& expected_invalidation_id) {
  try {
    const YAML::Node cfg = fs::exists(config_path) ? YAML::LoadFile(config_path.string()) : YAML::Node();
    HM_RETURN_IF_ERROR(validate_pending_stitching_invalidation(cfg, expected_invalidation_id));
    YAML::Node calibration;
    if (cfg && cfg.IsMap()) {
      const YAML::Node ui = cfg["hstream_ui"];
      if (ui && ui.IsMap())
        calibration = ui["stitching_calibration"];
    }
    return calibration && calibration["artifacts_invalidated"] && calibration["artifacts_invalidated"].IsScalar() &&
        calibration["artifacts_invalidated"].as<bool>();
  } catch (const YAML::Exception& ex) {
    return absl::InvalidArgumentError(
        TO_STRING("Failed to validate private config \"" << config_path.string() << "\": " << ex.what()));
  }
}

} // namespace

absl::StatusOr<bool> is_stitching_invalidation_cleanup_applied(
    const std::string& game_dir,
    const std::string& expected_invalidation_id) {
  if (game_dir.empty() || expected_invalidation_id.empty())
    return absl::InvalidArgumentError("Missing game directory or stitching invalidation ID");
  const fs::path game_dir_path(game_dir);
  auto config_transaction = GameConfigTransactionLock::Acquire(game_dir_path);
  if (!config_transaction.ok())
    return config_transaction.status();
  return read_stitching_invalidation_cleanup_state(game_dir_path / "config.yaml", expected_invalidation_id);
}

absl::Status clean_stitching_artifacts_impl(
    const std::string& game_dir,
    bool preserve_synchronized_inputs,
    const std::string& expected_invalidation_id) {
  if (game_dir.empty()) {
    return absl::InvalidArgumentError("Missing game directory");
  }

  const fs::path game_dir_path(game_dir);
  std::error_code ec;
  if (!fs::exists(game_dir_path, ec) || ec) {
    return absl::NotFoundError(TO_STRING("Game directory does not exist: " << game_dir));
  }

  // Every artifact owner follows Hugin -> config -> rink ordering. Holding all
  // three locks prevents clean from splitting a committed generation or
  // racing a config.yaml read-modify-write.
  auto hugin_lock = HuginProject::RecoverAndLock(game_dir_path);
  if (!hugin_lock.ok())
    return hugin_lock.status();
  auto config_transaction = GameConfigTransactionLock::Acquire(game_dir_path);
  if (!config_transaction.ok())
    return config_transaction.status();

  const fs::path cfg_file_path = game_dir_path / "config.yaml";
  if (!expected_invalidation_id.empty()) {
    bool artifacts_invalidated = false;
    HM_ASSIGN_OR_RETURN(
        artifacts_invalidated, read_stitching_invalidation_cleanup_state(cfg_file_path, expected_invalidation_id));
    if (artifacts_invalidated)
      return absl::AbortedError("Stitching cleanup invalidation was already applied before artifact deletion");
  }

  size_t removed_files = 0;
  auto clean_pattern = [&](const std::string& pattern) -> absl::Status {
    size_t removed = 0;
    HM_ASSIGN_OR_RETURN(removed, clean_files_matching(game_dir_path, pattern));
    removed_files += removed;
    return absl::OkStatus();
  };
  HM_RETURN_IF_ERROR(clean_pattern("hm_project.pto"));
  HM_RETURN_IF_ERROR(clean_pattern("autooptimiser_out.pto"));
  HM_RETURN_IF_ERROR(clean_pattern("*.pto"));
  HM_RETURN_IF_ERROR(clean_pattern("mapping_*.tif"));
  HM_RETURN_IF_ERROR(clean_pattern("mapping_*.tiff"));
  HM_RETURN_IF_ERROR(clean_pattern("stitching_canvas_provenance"));
  HM_RETURN_IF_ERROR(clean_pattern("panorama.tif"));
  HM_RETURN_IF_ERROR(clean_pattern("seam_file.png"));
  HM_RETURN_IF_ERROR(clean_pattern("matches.png"));
  HM_RETURN_IF_ERROR(clean_pattern("keypoints.png"));
  HM_RETURN_IF_ERROR(clean_pattern("s.png"));
  HM_RETURN_IF_ERROR(clean_pattern("rink_mask_*.png"));
  if (!preserve_synchronized_inputs) {
    HM_RETURN_IF_ERROR(clean_pattern("left.png"));
    HM_RETURN_IF_ERROR(clean_pattern("right.png"));
    size_t removed_extracted_frames = 0;
    HM_ASSIGN_OR_RETURN(removed_extracted_frames, delete_extracted_frames(game_dir_path));
    removed_files += removed_extracted_frames;
  }

  if (fs::exists(cfg_file_path)) {
    try {
      YAML::Node cfg = YAML::LoadFile(cfg_file_path.string());
      if (preserve_synchronized_inputs) {
        remove_control_point_dependent_cache_keys(cfg);
      } else {
        remove_cleanable_stitching_cache_keys(cfg);
      }
      std::ostringstream out;
      if (!is_empty_yaml_document(cfg)) {
        out << cfg << "\n";
      }
      HM_RETURN_IF_ERROR(publish_game_config(game_dir_path, out.str()));
    } catch (const YAML::Exception& ex) {
      return absl::InternalError(
          TO_STRING("Failed to clean private config \"" << cfg_file_path.string() << "\": " << ex.what()));
    } catch (...) {
      return absl::InternalError(
          TO_STRING("Unknown error while cleaning private config \"" << cfg_file_path.string() << '"'));
    }
  }

  if (removed_files) {
    std::cout << "Removed " << removed_files
              << (preserve_synchronized_inputs ? " control-point-dependent stitch artifact file(s) from \""
                                               : " stitch artifact file(s) from \"")
              << game_dir << "\"\n";
  }
  return absl::OkStatus();
}

absl::Status clean_stitching_artifacts(const std::string& game_dir, const std::string& expected_invalidation_id) {
  return clean_stitching_artifacts_impl(game_dir, /*preserve_synchronized_inputs=*/false, expected_invalidation_id);
}

absl::Status clean_stitching_artifacts_from_control_points(
    const std::string& game_dir,
    const std::string& expected_invalidation_id) {
  return clean_stitching_artifacts_impl(game_dir, /*preserve_synchronized_inputs=*/true, expected_invalidation_id);
}

static absl::StatusOr<bool> validate_stitching_artifacts_locked(
    const std::string& game_dir,
    size_t max_output_width,
    bool normalize_seam,
    const HuginProject::ArtifactLock& artifact_lock,
    CanvasSize* validated_canvas = nullptr) {
  bool up_to_date = test_dependency_tree(game_dir, /*add_rink_mask=*/false);
  if (!up_to_date) {
    return false;
  }
  CanvasSize canvas_size;
  TiffPlacement p0;
  TiffPlacement p1;
  auto p0_status = read_tiff_placement(fs::path(game_dir) / "mapping_0000.tif");
  auto p1_status = read_tiff_placement(fs::path(game_dir) / "mapping_0001.tif");
  if (!p0_status.ok() || !p1_status.ok()) {
    std::cout << "Stitching artifacts exist but mapping TIFF placement metadata is invalid: "
              << (!p0_status.ok() ? p0_status.status() : p1_status.status()) << std::endl;
    return false;
  }
  p0 = *p0_status;
  p1 = *p1_status;
  auto canvas_status = normalize_and_measure_canvas(&p0, &p1);
  if (!canvas_status.ok()) {
    std::cout << "Stitching artifacts exist but mapping canvas metadata is invalid: " << canvas_status.status()
              << std::endl;
    return false;
  }
  canvas_size = *canvas_status;
  auto provenance = HuginProject::ReadCanvasProvenance(game_dir, artifact_lock);
  if (!provenance.ok()) {
    if (absl::IsFailedPrecondition(provenance.status()) || absl::IsInvalidArgument(provenance.status()) ||
        absl::IsNotFound(provenance.status())) {
      std::cout << "Stitching canvas provenance is invalid: " << provenance.status() << std::endl;
      return false;
    }
    return provenance.status();
  }
  const auto compatibility = check_canvas_provenance_compatibility(
      *provenance, canvas_size, max_output_width, live_stitch_max_canvas_dimension());
  if (!compatibility.compatible) {
    std::cout << "Stitching artifacts require mapping regeneration because " << compatibility.reason << std::endl;
    return false;
  }
  const absl::Status remap_status = validate_remap_artifact_headers(fs::path(game_dir), p0, p1);
  if (absl::IsFailedPrecondition(remap_status) || absl::IsInvalidArgument(remap_status) ||
      absl::IsNotFound(remap_status) || absl::IsResourceExhausted(remap_status)) {
    std::cout << "Stitching artifacts exist but remap TIFF metadata is invalid: " << remap_status << std::endl;
    return false;
  }
  HM_RETURN_IF_ERROR(remap_status);
  const absl::Status seam_status = normalize_seam ? HuginProject::ValidateAndNormalizeSeam(
                                                        fs::path(game_dir) / "seam_file.png",
                                                        static_cast<int>(canvas_size.width),
                                                        static_cast<int>(canvas_size.height),
                                                        static_cast<int>(canvas_size.width),
                                                        static_cast<int>(canvas_size.height),
                                                        1.0)
                                                  : HuginProject::ValidateSeamForConfiguredArtifacts(
                                                        fs::path(game_dir) / "seam_file.png",
                                                        static_cast<int>(canvas_size.width),
                                                        static_cast<int>(canvas_size.height),
                                                        static_cast<int>(canvas_size.width),
                                                        static_cast<int>(canvas_size.height));
  if (absl::IsFailedPrecondition(seam_status)) {
    std::cout << "Stitching artifacts exist but seam_file.png does not match the requested canvas: " << seam_status
              << std::endl;
    return false;
  }
  HM_RETURN_IF_ERROR(seam_status);
  if (validated_canvas != nullptr)
    *validated_canvas = canvas_size;
  return true;
}

absl::StatusOr<bool> is_stitching_configured(const std::string& game_dir, size_t max_output_width) {
  std::error_code directory_error;
  if (!fs::is_directory(game_dir, directory_error) || directory_error)
    return false;
  auto artifact_lock = HuginProject::RecoverAndLock(game_dir);
  if (!artifact_lock.ok())
    return artifact_lock.status();
  return validate_stitching_artifacts_locked(game_dir, max_output_width, /*normalize_seam=*/false, **artifact_lock);
}

absl::StatusOr<LockedStitchingArtifacts> lock_validated_stitching_artifacts(
    const std::string& game_dir,
    size_t max_output_width) {
  std::error_code directory_error;
  if (!fs::is_directory(game_dir, directory_error) || directory_error)
    return LockedStitchingArtifacts{};
  auto artifact_lock = HuginProject::RecoverAndLock(game_dir);
  if (!artifact_lock.ok())
    return artifact_lock.status();
  bool configured = false;
  CanvasSize canvas_size;
  HM_ASSIGN_OR_RETURN(
      configured,
      validate_stitching_artifacts_locked(
          game_dir, max_output_width, /*normalize_seam=*/true, **artifact_lock, &canvas_size));
  if (!configured)
    return LockedStitchingArtifacts{};
  std::string generation_id;
  HM_ASSIGN_OR_RETURN(generation_id, HuginProject::GenerationId(game_dir, **artifact_lock));
  return LockedStitchingArtifacts{
      .artifact_lock = std::move(*artifact_lock),
      .canvas_size = {.width = canvas_size.width, .height = canvas_size.height},
      .generation_id = std::move(generation_id),
  };
}

absl::StatusOr<bool> stitching_artifacts_exceed_live_canvas_limit(
    const std::string& game_dir,
    size_t max_output_width) {
  auto artifact_lock = HuginProject::RecoverAndLock(game_dir);
  if (!artifact_lock.ok())
    return artifact_lock.status();
  bool up_to_date = test_dependency_tree(game_dir, /*add_rink_mask=*/false);
  if (!up_to_date) {
    return false;
  }
  const auto max_canvas_dimension = live_stitch_max_canvas_dimension();
  if (!max_canvas_dimension.has_value()) {
    return false;
  }
  CanvasSize canvas_size;
  HM_ASSIGN_OR_RETURN(canvas_size, get_mapping_canvas_size(fs::path(game_dir)));
  CanvasSize effective_canvas_size = canvas_size;
  if (max_output_width > 0) {
    HM_ASSIGN_OR_RETURN(effective_canvas_size, get_effective_mapping_canvas_size(fs::path(game_dir), max_output_width));
  }
  return artifacts_exceed_live_canvas_limit(canvas_size, effective_canvas_size, *max_canvas_dimension);
}

absl::StatusOr<LockedCanvasRegenerationCheck> lock_canvas_regeneration_check(
    const std::string& game_dir,
    size_t max_output_width) {
  auto artifact_lock = HuginProject::RecoverAndLock(game_dir);
  if (!artifact_lock.ok())
    return artifact_lock.status();
  auto compatibility = check_canvas_constraint_locked(game_dir, max_output_width);
  if (!compatibility.ok())
    return compatibility.status();
  return LockedCanvasRegenerationCheck{
      .artifact_lock = std::move(*artifact_lock),
      .artifacts_compatible = compatibility->artifacts_compatible,
      .requires_regeneration = compatibility->requires_regeneration,
  };
}

absl::StatusOr<bool> stitching_artifacts_require_canvas_regeneration(
    const std::string& game_dir,
    size_t max_output_width) {
  LockedCanvasRegenerationCheck check;
  HM_ASSIGN_OR_RETURN(check, lock_canvas_regeneration_check(game_dir, max_output_width));
  return check.requires_regeneration;
}

absl::StatusOr<StitchingCanvasSize> stitching_canvas_size(const std::string& game_dir, size_t max_output_width) {
  if (game_dir.empty())
    return absl::InvalidArgumentError("A game directory is required");
  auto artifact_lock = HuginProject::RecoverAndLock(game_dir);
  if (!artifact_lock.ok())
    return artifact_lock.status();
  CanvasSize canvas_size;
  if (max_output_width > 0) {
    HM_ASSIGN_OR_RETURN(canvas_size, get_effective_mapping_canvas_size(fs::path(game_dir), max_output_width));
  } else {
    HM_ASSIGN_OR_RETURN(canvas_size, get_mapping_canvas_size(fs::path(game_dir)));
  }
  return StitchingCanvasSize{.width = canvas_size.width, .height = canvas_size.height};
}

absl::Status maybe_create_default_seam_file(const std::string& game_dir, size_t max_output_width) {
  if (game_dir.empty()) {
    return absl::InvalidArgumentError("Game dir is empty");
  }

  const fs::path root = fs::path(game_dir);
  auto artifact_lock = HuginProject::RecoverAndLock(root);
  if (!artifact_lock.ok())
    return artifact_lock.status();
  const fs::path seam_path = root / "seam_file.png";

  const fs::path mapping0_path = root / "mapping_0000.tif";
  const fs::path mapping1_path = root / "mapping_0001.tif";
  if (!fs::exists(mapping0_path) || !fs::exists(mapping1_path)) {
    return absl::NotFoundError(
        TO_STRING("Cannot create default seam_file.png; missing mapping TIFFs under " << root.string()));
  }

  TiffPlacement p0;
  TiffPlacement p1;
  HM_ASSIGN_OR_RETURN(p0, read_tiff_placement(mapping0_path));
  HM_ASSIGN_OR_RETURN(p1, read_tiff_placement(mapping1_path));

  CanvasSize measured_canvas;
  HM_ASSIGN_OR_RETURN(measured_canvas, normalize_and_measure_canvas(&p0, &p1));
  CanvasSize effective_canvas = measured_canvas;
  double selected_scale = 1.0;
  if (max_output_width > 0) {
    const ScaledCanvas scaled = scale_canvas_to_max_output_width(p0, p1, measured_canvas, max_output_width);
    effective_canvas = scaled.size;
    selected_scale = scaled.scale;
  }
  const int canvas_width = static_cast<int>(effective_canvas.width);
  const int canvas_height = static_cast<int>(effective_canvas.height);
  const absl::Status seam_status = HuginProject::ValidateAndNormalizeSeam(
      seam_path,
      static_cast<int>(measured_canvas.width),
      static_cast<int>(measured_canvas.height),
      canvas_width,
      canvas_height,
      selected_scale);
  if (seam_status.ok())
    return absl::OkStatus();
  if (!absl::IsFailedPrecondition(seam_status))
    return seam_status;

  if (!hard_seam_fallback_enabled()) {
    return absl::FailedPreconditionError(TO_STRING(
        "A usable seam_file.png is required under " << root.string()
                                                    << "; refusing to generate a hard-seam fallback: " << seam_status
                                                    << ". Regenerate it with enblend or set "
                                                       "HM_ALLOW_HARD_SEAM_FALLBACK=1 to explicitly "
                                                       "allow the fallback"));
  }
  std::cerr << "Existing seam mask is unusable; HM_ALLOW_HARD_SEAM_FALLBACK=1 permits regeneration: " << seam_status
            << std::endl;

  const int x0 = static_cast<int>(std::floor(p0.x_px * selected_scale));
  const int y0 = static_cast<int>(std::floor(p0.y_px * selected_scale));
  const int x1 = static_cast<int>(std::floor(p1.x_px * selected_scale));
  const int y1 = static_cast<int>(std::floor(p1.y_px * selected_scale));

  const int x0_end = static_cast<int>(std::ceil((p0.x_px + p0.width) * selected_scale));
  const int x1_end = static_cast<int>(std::ceil((p1.x_px + p1.width) * selected_scale));

  const int overlap_start = std::max(x0, x1);
  const int overlap_end = std::min(x0_end, x1_end);

  int seam_x = canvas_width / 2;
  if (overlap_end > overlap_start) {
    seam_x = overlap_start + (overlap_end - overlap_start) / 2;
  } else if (x1 > x0) {
    seam_x = x1;
  }
  seam_x = std::clamp(seam_x, 0, canvas_width);

  // NOTE: hm-cupano inverts the seam mask at load time and uses 1 for image 0 (left) and 0 for image 1 (right).
  // Creating {0,255} (then inverted) yields {1,0} respectively.
  cv::Mat mask;
  try {
    mask = cv::Mat(canvas_height, canvas_width, CV_8U, cv::Scalar(0));
  } catch (const cv::Exception& exception) {
    return absl::ResourceExhaustedError("Unable to allocate fallback seam: " + std::string(exception.what()));
  } catch (const std::bad_alloc&) {
    return absl::ResourceExhaustedError("Unable to allocate fallback seam");
  }
  if (seam_x < canvas_width) {
    mask.colRange(seam_x, canvas_width).setTo(255);
  }

  const int y0_end = static_cast<int>(std::ceil((p0.y_px + p0.height) * selected_scale));
  const int y1_end = static_cast<int>(std::ceil((p1.y_px + p1.height) * selected_scale));
  const int x0_clamped = std::clamp(x0, 0, canvas_width);
  const int x0_end_clamped = std::clamp(x0_end, 0, canvas_width);
  const int x1_clamped = std::clamp(x1, 0, canvas_width);
  const int x1_end_clamped = std::clamp(x1_end, 0, canvas_width);

  for (int y = 0; y < canvas_height; ++y) {
    const bool in0 = y >= y0 && y < y0_end;
    const bool in1 = y >= y1 && y < y1_end;
    if (!in0 && !in1) {
      continue;
    }

    uint8_t* row = mask.ptr<uint8_t>(y);
    if (in0 && !in1) {
      std::fill(row + x0_clamped, row + x0_end_clamped, static_cast<uint8_t>(0));
    } else if (in1 && !in0) {
      std::fill(row + x1_clamped, row + x1_end_clamped, static_cast<uint8_t>(255));
    }
  }

  try {
    if (!cv::imwrite(seam_path.string(), mask)) {
      return absl::InternalError(TO_STRING("Failed to write seam mask: " << seam_path.string()));
    }
  } catch (const cv::Exception& exception) {
    return absl::InternalError("Unable to encode fallback seam: " + std::string(exception.what()));
  }

  std::cout << "Created fallback seam mask: " << seam_path.string() << " (" << canvas_width << "x" << canvas_height
            << ")" << std::endl;
  return HuginProject::ValidateAndNormalizeSeam(seam_path, canvas_width, canvas_height);
}

bool can_configure_stitching(const YAML::Node& config) {
  return true;
}

std::string read_scalar_or_default(
    const YAML::Node& root,
    const std::initializer_list<std::string>& path,
    const std::string& fallback) {
  YAML::Node current = root;
  for (const std::string& key : path) {
    if (!current || !current.IsMap())
      return fallback;
    current = current[key];
  }
  return current && current.IsScalar() ? current.as<std::string>() : fallback;
}

absl::StatusOr<Synchronization> calculate_stitching_synchronization(
    const std::string& video1,
    const std::string& video2) {
  auto frame_offsets = synchronize_by_audio(video1, video2);
  return Synchronization{
      .video1_frame_offset = frame_offsets.first,
      .video2_frame_offset = frame_offsets.second,
  };
}

absl::Status create_control_points(
    const std::string& game_dir,
    const std::vector<StitchingCalibrationFramePair>& frame_pairs,
    const std::string& expected_invalidation_id,
    const std::function<bool()>& is_cancelled,
    size_t max_output_width) {
  if (frame_pairs.empty()) {
    return absl::InvalidArgumentError("Stitching calibration requires at least one synchronized frame pair");
  }
  std::string pattern = (fs::path(game_dir) / ".hstream-calibration-input-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  char* created = ::mkdtemp(writable.data());
  if (created == nullptr) {
    return absl::InternalError(
        TO_STRING("Unable to create private calibration input directory: " << std::strerror(errno)));
  }
  const fs::path input_dir(created);
  struct RemoveInputDirectory {
    fs::path path;
    ~RemoveInputDirectory() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } input_cleanup{input_dir};
  if (::chmod(input_dir.c_str(), 0700) != 0) {
    return absl::InternalError("Unable to protect private calibration input directory");
  }

  std::vector<std::pair<fs::path, fs::path>> input_files;
  input_files.reserve(frame_pairs.size());
  for (size_t index = 0; index < frame_pairs.size(); ++index) {
    const fs::path left_file = input_dir /
        (index == 0 ? "left.png" : TO_STRING("left_" << std::setw(4) << std::setfill('0') << index << ".png"));
    const fs::path right_file = input_dir /
        (index == 0 ? "right.png" : TO_STRING("right_" << std::setw(4) << std::setfill('0') << index << ".png"));
    HM_RETURN_IF_ERROR(save_image(frame_pairs[index].left, left_file));
    HM_RETURN_IF_ERROR(save_image(frame_pairs[index].right, right_file));
    input_files.emplace_back(left_file, right_file);
  }
  size_t max_control_points = utils::getenv("HM_MAX_CONTROL_POINTS", kDefaultMaxControlPoints);
  const auto max_canvas_dimension = live_stitch_max_canvas_dimension();
  ControlPointMatcher control_point_matcher = ControlPointMatcher::kSuperPointLightGlue;
  MappingBackend mapping_backend = MappingBackend::kNona;
  const fs::path game_config_path = fs::path(game_dir) / "config.yaml";
  try {
    if (fs::exists(game_config_path)) {
      const YAML::Node config = YAML::LoadFile(game_config_path.string());
      HM_ASSIGN_OR_RETURN(
          control_point_matcher,
          ParseControlPointMatcher(read_scalar_or_default(
              config, {"stitching", "control_point_matcher"}, ControlPointMatcherName(control_point_matcher))));
      HM_ASSIGN_OR_RETURN(
          mapping_backend,
          ParseMappingBackend(
              read_scalar_or_default(config, {"stitching", "mapping_backend"}, MappingBackendName(mapping_backend))));
    }
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(TO_STRING(
        "Failed to read stitching backend choices from " << game_config_path.string() << ": " << exception.what()));
  }

  report_calibration_progress(
      "features",
      "started",
      TO_STRING(
          "Looking for control points in " << input_files.size() << " synchronized camera frame pair"
                                           << (input_files.size() == 1 ? "" : "s")));
  fs::path model_path;
  HM_ASSIGN_OR_RETURN(model_path, feature_matcher_model_path());
  std::unique_ptr<FeatureMatcher> matcher;
  HM_ASSIGN_OR_RETURN(matcher, FeatureMatcher::Create(model_path.string(), control_point_matcher));
  struct CandidateFramePair {
    size_t index{0};
    std::vector<FeatureMatch> accepted;
  };
  std::vector<FeatureMatch> pooled_accepted;
  std::vector<CandidateFramePair> candidates;
  cv::Size left_source_size;
  cv::Size right_source_size;
  size_t matched_frame_pairs = 0;
  size_t skipped_frame_pairs = 0;
  for (size_t index = 0; index < input_files.size(); ++index) {
    auto left_or = load_feature_image(input_files[index].first);
    if (!left_or.ok())
      return left_or.status();
    auto right_or = load_feature_image(input_files[index].second);
    if (!right_or.ok())
      return right_or.status();
    cv::Mat left = std::move(*left_or);
    cv::Mat right = std::move(*right_or);
    if (index == 0) {
      left_source_size = left.size();
      right_source_size = right.size();
    } else if (left.size() != left_source_size || right.size() != right_source_size) {
      return absl::FailedPreconditionError("Stitching calibration frame pairs must have stable input dimensions");
    }
    auto frame_matches_or = matcher->Infer(left, right, max_control_points, {}, is_cancelled);
    if (!frame_matches_or.ok()) {
      if (absl::IsNotFound(frame_matches_or.status())) {
        ++skipped_frame_pairs;
        std::cerr << "Skipping stitching calibration frame pair " << (index + 1) << "/" << input_files.size() << ": "
                  << frame_matches_or.status() << std::endl;
        continue;
      }
      return frame_matches_or.status();
    }
    const FeatureMatchResult frame_matches = std::move(*frame_matches_or);
    ++matched_frame_pairs;
    candidates.push_back(CandidateFramePair{.index = index, .accepted = frame_matches.accepted});
    pooled_accepted.insert(pooled_accepted.end(), frame_matches.accepted.begin(), frame_matches.accepted.end());
  }
  report_calibration_progress(
      "features",
      "complete",
      TO_STRING(
          "Control points found in " << matched_frame_pairs << "/" << input_files.size()
                                     << " synchronized camera frame pair" << (input_files.size() == 1 ? "" : "s")));
  report_calibration_progress("matching", "started", "Selecting and validating control-point matches");
  if (is_cancelled && is_cancelled()) {
    return absl::CancelledError("Stitching calibration cancelled");
  }
  if (pooled_accepted.size() < 16) {
    return absl::FailedPreconditionError(TO_STRING(
        "Native feature matcher produced only " << pooled_accepted.size() << " usable matches across "
                                                << matched_frame_pairs << "/" << input_files.size()
                                                << " frame pairs; at least 16 are required"));
  }
  if (matched_frame_pairs == 0) {
    return absl::FailedPreconditionError("No stitching calibration frame pair produced usable matches");
  }
  std::stable_sort(
      candidates.begin(), candidates.end(), [](const CandidateFramePair& lhs, const CandidateFramePair& rhs) {
        return lhs.accepted.size() > rhs.accepted.size();
      });
  report_calibration_progress(
      "matching",
      "complete",
      TO_STRING(
          "Matched candidates from " << pooled_accepted.size() << " usable control points across "
                                     << matched_frame_pairs << "/" << input_files.size() << " frame pair"
                                     << (input_files.size() == 1 ? "" : "s")
                                     << (skipped_frame_pairs == 0 ? "" : TO_STRING(", skipped " << skipped_frame_pairs))
                                     << ")"));

  HuginProject::Options options;
  options.max_canvas_dimension = max_canvas_dimension;
  if (max_output_width > 0)
    options.max_output_width = max_output_width;
  options.mapping_backend = mapping_backend;
  options.expected_invalidation_id = expected_invalidation_id;
  options.progress = report_calibration_progress;
  options.is_cancelled = is_cancelled;
  absl::Status last_candidate_status =
      absl::FailedPreconditionError("No stitching calibration frame pair had enough usable matches");
  size_t attempted_candidates = 0;
  for (const CandidateFramePair& candidate : candidates) {
    if (candidate.accepted.size() < 16) {
      continue;
    }
    auto selected_or = FeatureMatcher::SelectControlPoints(candidate.accepted, left_source_size, max_control_points);
    if (!selected_or.ok()) {
      last_candidate_status = selected_or.status();
      if (absl::IsFailedPrecondition(last_candidate_status) || absl::IsNotFound(last_candidate_status)) {
        continue;
      }
      return last_candidate_status;
    }
    std::vector<FeatureMatch> selected = std::move(*selected_or);
    for (const FeatureMatch& match : selected) {
      if (match.left.x < 0.0f || match.left.y < 0.0f || match.right.x < 0.0f || match.right.y < 0.0f ||
          match.left.x >= left_source_size.width || match.left.y >= left_source_size.height ||
          match.right.x >= right_source_size.width || match.right.y >= right_source_size.height) {
        return absl::FailedPreconditionError("Selected control point falls outside the representative Hugin images");
      }
    }
    ++attempted_candidates;
    std::cerr << "Trying stitching calibration frame pair " << (candidate.index + 1) << "/" << input_files.size()
              << " with " << selected.size() << " selected control points" << std::endl;
    absl::Status configure_status = HuginProject::Configure(
        game_dir, input_files[candidate.index].first, input_files[candidate.index].second, selected, options);
    if (configure_status.ok()) {
      return absl::OkStatus();
    }
    last_candidate_status = configure_status;
    if (absl::IsCancelled(configure_status) || absl::IsAborted(configure_status) ||
        absl::IsInvalidArgument(configure_status) || absl::IsInternal(configure_status) ||
        absl::IsResourceExhausted(configure_status)) {
      return configure_status;
    }
    if (!absl::IsFailedPrecondition(configure_status) && !absl::IsNotFound(configure_status)) {
      return configure_status;
    }
    std::cerr << "Skipping stitching calibration frame pair " << (candidate.index + 1) << "/" << input_files.size()
              << ": " << configure_status << std::endl;
  }
  return absl::FailedPreconditionError(TO_STRING(
      "No stitching calibration frame pair produced a usable Hugin solution after "
      << attempted_candidates << " candidate attempt" << (attempted_candidates == 1 ? "" : "s") << ": "
      << last_candidate_status));
}

namespace {

constexpr const char* kRinkTransactionPrefix = ".hstream-rink-";

absl::Status fsync_path(const fs::path& path, bool directory) {
  const int flags = O_RDONLY | O_CLOEXEC | (directory ? O_DIRECTORY : 0);
  const int descriptor = ::open(path.c_str(), flags);
  if (descriptor < 0)
    return absl::InternalError("Unable to open artifact for fsync: " + path.string());
  const int result = ::fsync(descriptor);
  const std::string message = result == 0 ? std::string() : std::strerror(errno);
  ::close(descriptor);
  if (result != 0)
    return absl::InternalError("Unable to fsync artifact " + path.string() + ": " + message);
  return absl::OkStatus();
}

absl::Status write_transaction_file(const fs::path& path, const std::string& contents) {
  std::ofstream output(path, std::ios::out | std::ios::trunc);
  if (!output)
    return absl::InternalError("Unable to write rink transaction file: " + path.string());
  output << contents;
  output.flush();
  if (!output)
    return absl::InternalError("Unable to flush rink transaction file: " + path.string());
  output.close();
  return fsync_path(path);
}

absl::Status mark_rink_transaction_rolled_back(const fs::path& transaction) {
  const fs::path temporary = transaction / "state.rolled_back";
  HM_RETURN_IF_ERROR(write_transaction_file(temporary, "ROLLED_BACK\n"));
  std::error_code error;
  fs::rename(temporary, transaction / "state", error);
  if (error)
    return absl::InternalError("Unable to commit rink rollback state: " + error.message());
  return fsync_path(transaction, true);
}

bool is_rink_artifact_name(const std::string& name) {
  static const std::regex mask_pattern(R"(^rink_mask_(0|[1-9][0-9]*)[.]png$)");
  return name == "config.yaml" || name == "s.png" || std::regex_match(name, mask_pattern);
}

absl::StatusOr<std::string> read_rink_transaction_state(const fs::path& transaction) {
  const fs::path state_path = transaction / "state";
  std::error_code error;
  const bool exists = fs::exists(state_path, error);
  if (error)
    return absl::InternalError("Unable to inspect rink transaction state: " + error.message());
  if (!exists)
    return std::string("UNPREPARED");
  const int descriptor = ::open(state_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0)
    return absl::FailedPreconditionError("Unable to open durable rink transaction state");
  struct StateFileCleanup {
    int descriptor;
    ~StateFileCleanup() {
      ::close(descriptor);
    }
  } cleanup{descriptor};
  struct stat metadata{};
  if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
      metadata.st_size > 16) {
    return absl::FailedPreconditionError("Invalid durable rink transaction state file");
  }
  std::string contents(static_cast<size_t>(metadata.st_size), '\0');
  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t count = ::read(descriptor, contents.data() + offset, contents.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return absl::FailedPreconditionError("Unable to read durable rink transaction state");
    offset += static_cast<size_t>(count);
  }
  if (contents == "PREPARED\n")
    return std::string("PREPARED");
  if (contents == "COMMITTED\n")
    return std::string("COMMITTED");
  if (contents == "ROLLED_BACK\n")
    return std::string("ROLLED_BACK");
  return absl::FailedPreconditionError("Invalid durable rink transaction state contents");
}

absl::StatusOr<std::set<std::string>> read_rink_manifest(const fs::path& transaction) {
  const fs::path path = transaction / "new-files";
  std::error_code error;
  if (!fs::is_regular_file(path, error) || error)
    return absl::FailedPreconditionError("Prepared rink transaction has no readable new-files manifest");
  if (fs::file_size(path, error) > 64 * 1024 || error)
    return absl::FailedPreconditionError("Prepared rink transaction manifest is too large");
  std::ifstream input(path);
  if (!input)
    return absl::FailedPreconditionError("Unable to open prepared rink transaction manifest");
  std::set<std::string> names;
  std::string name;
  while (input >> name) {
    if (fs::path(name).filename() != name || !is_rink_artifact_name(name) || !names.insert(name).second)
      return absl::InvalidArgumentError("Invalid rink transaction filename: " + name);
  }
  if (!input.eof() || !names.count("config.yaml") || names.size() < 2)
    return absl::FailedPreconditionError("Prepared rink transaction manifest is incomplete");
  return names;
}

absl::Status recover_rink_transactions_locked(const fs::path& root) {
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(root, error)) {
    if (error)
      return absl::InternalError("Unable to inspect rink transactions: " + error.message());
    const std::string directory_name = entry.path().filename().string();
    if (!entry.is_directory(error) || error || directory_name.rfind(kRinkTransactionPrefix, 0) != 0) {
      error.clear();
      continue;
    }
    const fs::path transaction = entry.path();
    auto state = read_rink_transaction_state(transaction);
    if (!state.ok())
      return state.status();
    if (*state == "PREPARED") {
      auto manifest = read_rink_manifest(transaction);
      if (!manifest.ok())
        return manifest.status();
      const fs::path previous = transaction / "previous";
      std::vector<fs::path> backups;
      const bool previous_exists = fs::exists(previous, error);
      if (error)
        return absl::InternalError("Unable to inspect rink transaction backup directory: " + error.message());
      if (previous_exists) {
        if (!fs::is_directory(previous, error) || error)
          return absl::FailedPreconditionError("Rink transaction backup is not a directory");
        for (const auto& old : fs::directory_iterator(previous, error)) {
          if (error)
            return absl::InternalError("Unable to inspect rink transaction backup: " + error.message());
          const std::string old_name = old.path().filename().string();
          if (!old.is_regular_file(error) || error || !is_rink_artifact_name(old_name))
            return absl::InvalidArgumentError("Invalid rink transaction backup: " + old_name);
          backups.push_back(old.path());
        }
      }
      for (const std::string& name : *manifest) {
        fs::remove(root / name, error);
        if (error)
          return absl::InternalError("Unable to remove interrupted rink artifact: " + error.message());
      }
      size_t restored = 0;
      for (const fs::path& old : backups) {
        const fs::path destination = root / old.filename();
        auto restore = link_rink_rollback_file(old, destination);
        if (!restore.ok())
          return restore;
        auto status = fsync_path(destination);
        if (!status.ok())
          return status;
        ++restored;
        if (const char* fail_after = std::getenv("HM_TEST_RINK_ROLLBACK_FAIL_AFTER");
            fail_after != nullptr && restored == static_cast<size_t>(std::strtoull(fail_after, nullptr, 10))) {
          return absl::InternalError("Injected rink rollback interruption");
        }
      }
      error.clear();
      auto status = fsync_path(root, true);
      if (!status.ok())
        return status;
      status = mark_rink_transaction_rolled_back(transaction);
      if (!status.ok())
        return status;
    }
    // COMMITTED transactions already have a durable new generation. An
    // UNPREPARED directory has no publication metadata and never changed a
    // root artifact.
    fs::remove_all(transaction, error);
    if (error)
      return absl::InternalError("Unable to clean rink transaction: " + error.message());
  }
  return fsync_path(root, true);
}

} // namespace

absl::StatusOr<std::string> stitched_output_generation_id(
    const std::string& hugin_generation,
    double post_stitch_rotate_degrees,
    size_t output_width,
    size_t output_height) {
  if (hugin_generation.empty())
    return absl::InvalidArgumentError("A Hugin generation is required");
  if (!std::isfinite(post_stitch_rotate_degrees))
    return absl::InvalidArgumentError("Post-stitch rotation must be finite");
  if ((output_width == 0) != (output_height == 0))
    return absl::InvalidArgumentError("Stitched output dimensions must both be set or both be omitted");
  if (post_stitch_rotate_degrees == 0.0)
    post_stitch_rotate_degrees = 0.0;
  std::ostringstream generation;
  generation.imbue(std::locale::classic());
  generation << std::setprecision(std::numeric_limits<double>::max_digits10);
  generation << "hstream-stitched-output-v1\nhugin-bytes:" << hugin_generation.size() << '\n'
             << hugin_generation << "post-stitch-rotate-degrees:" << post_stitch_rotate_degrees << '\n';
  if (output_width > 0) {
    generation << "output-size:" << output_width << 'x' << output_height << '\n';
  }
  return generation.str();
}

namespace {

absl::StatusOr<double> configured_post_stitch_rotation(const YAML::Node& config) {
  try {
    if (!config || !config.IsMap())
      return 0.0;
    YAML::Node stitching;
    for (const auto& entry : config) {
      if (entry.first.IsScalar() && entry.first.as<std::string>() == "stitching") {
        stitching = entry.second;
        break;
      }
    }
    if (!stitching || !stitching.IsMap())
      return 0.0;
    YAML::Node rotation;
    for (const auto& entry : stitching) {
      if (entry.first.IsScalar() && entry.first.as<std::string>() == "post_stitch_rotate_degrees") {
        rotation = entry.second;
        break;
      }
    }
    if (!rotation || !rotation.IsDefined() || rotation.IsNull())
      return 0.0;
    if (!rotation.IsScalar())
      return absl::InvalidArgumentError("Configured post-stitch rotation must be a scalar");
    const double value = rotation.as<double>();
    if (!std::isfinite(value))
      return absl::InvalidArgumentError("Configured post-stitch rotation must be finite");
    return value;
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to read configured post-stitch rotation: " + std::string(exception.what()));
  }
}

absl::StatusOr<std::optional<double>> stitched_output_persisted_rotation_marker(const YAML::Node& config) {
  try {
    if (!config || !config.IsMap())
      return std::nullopt;
    const std::optional<YAML::Node> rink = map_child(config, "rink");
    if (!rink.has_value() || !rink->IsMap())
      return std::nullopt;
    const std::optional<YAML::Node> marker = map_child(*rink, "stitched_output_persisted_rotation_degrees");
    if (!marker.has_value() || !marker->IsDefined())
      return std::nullopt;
    if (!marker->IsScalar())
      return absl::InvalidArgumentError("Stitched-output persisted-rotation marker must be a scalar");
    const double rotation = marker->as<double>();
    if (!std::isfinite(rotation))
      return absl::InvalidArgumentError("Stitched-output persisted-rotation marker must be finite");
    return rotation;
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to read stitched-output persisted-rotation marker: " + std::string(exception.what()));
  }
}

bool stitching_calibration_is_pending(const YAML::Node& config) {
  try {
    if (!config || !config.IsMap())
      return false;
    const std::optional<YAML::Node> ui = map_child(config, "hstream_ui");
    if (!ui.has_value() || !ui->IsMap())
      return false;
    const std::optional<YAML::Node> calibration = map_child(*ui, "stitching_calibration");
    if (!calibration.has_value() || !calibration->IsMap())
      return false;
    const std::optional<YAML::Node> status = map_child(*calibration, "status");
    return status.has_value() && status->IsScalar() && status->as<std::string>() == "pending";
  } catch (const YAML::Exception&) {
    return false;
  }
}

absl::StatusOr<std::string> configured_output_generation(
    const YAML::Node& config,
    const std::string& hugin_generation) {
  double rotation = 0.0;
  HM_ASSIGN_OR_RETURN(rotation, configured_post_stitch_rotation(config));
  return stitched_output_generation_id(hugin_generation, rotation);
}

absl::StatusOr<std::string> configured_output_generation(
    const YAML::Node& config,
    const std::string& hugin_generation,
    const CanvasSize& output_size) {
  double rotation = 0.0;
  HM_ASSIGN_OR_RETURN(rotation, configured_post_stitch_rotation(config));
  return stitched_output_generation_id(hugin_generation, rotation, output_size.width, output_size.height);
}

absl::Status validate_output_generation_hugin(
    const std::string& output_generation,
    const std::string& expected_hugin_generation) {
  constexpr std::string_view prefix = "hstream-stitched-output-v1\nhugin-bytes:";
  constexpr std::string_view rotation_prefix = "post-stitch-rotate-degrees:";
  if (output_generation.compare(0, prefix.size(), prefix) != 0)
    return absl::InvalidArgumentError("Invalid stitched-output generation header");
  const size_t length_end = output_generation.find('\n', prefix.size());
  if (length_end == std::string::npos || length_end == prefix.size())
    return absl::InvalidArgumentError("Invalid stitched-output Hugin length");
  size_t hugin_size = 0;
  for (size_t index = prefix.size(); index < length_end; ++index) {
    const unsigned char character = static_cast<unsigned char>(output_generation[index]);
    if (!std::isdigit(character))
      return absl::InvalidArgumentError("Invalid stitched-output Hugin length");
    const size_t digit = static_cast<size_t>(character - '0');
    if (hugin_size > (std::numeric_limits<size_t>::max() - digit) / 10)
      return absl::InvalidArgumentError("Stitched-output Hugin length is too large");
    hugin_size = hugin_size * 10 + digit;
  }
  const size_t hugin_start = length_end + 1;
  if (hugin_size > output_generation.size() - hugin_start)
    return absl::InvalidArgumentError("Truncated stitched-output Hugin generation");
  if (output_generation.compare(hugin_start, hugin_size, expected_hugin_generation) != 0 ||
      hugin_size != expected_hugin_generation.size()) {
    return absl::AbortedError("Stitched output uses a stale Hugin generation");
  }
  const size_t rotation_start = hugin_start + hugin_size;
  if (output_generation.compare(rotation_start, rotation_prefix.size(), rotation_prefix) != 0)
    return absl::InvalidArgumentError("Invalid stitched-output rotation field");
  const size_t value_start = rotation_start + rotation_prefix.size();
  const size_t value_end = output_generation.find('\n', value_start);
  if (value_end == std::string::npos || value_end == value_start)
    return absl::InvalidArgumentError("Invalid stitched-output rotation value");
  const std::string value = output_generation.substr(value_start, value_end - value_start);
  std::istringstream parser(value);
  parser.imbue(std::locale::classic());
  double rotation = 0.0;
  parser >> rotation;
  if (!parser || !std::isfinite(rotation))
    return absl::InvalidArgumentError("Invalid stitched-output rotation value");
  parser >> std::ws;
  if (!parser.eof())
    return absl::InvalidArgumentError("Invalid stitched-output rotation value");
  if (value_end + 1 < output_generation.size()) {
    constexpr std::string_view output_size_prefix = "output-size:";
    const size_t output_size_start = value_end + 1;
    if (output_generation.compare(output_size_start, output_size_prefix.size(), output_size_prefix) != 0 ||
        output_generation.back() != '\n') {
      return absl::InvalidArgumentError("Invalid stitched-output generation trailer");
    }
    const std::string dimensions = output_generation.substr(
        output_size_start + output_size_prefix.size(),
        output_generation.size() - output_size_start - output_size_prefix.size() - 1);
    const size_t separator = dimensions.find('x');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= dimensions.size())
      return absl::InvalidArgumentError("Invalid stitched-output dimensions");
    HM_RETURN_IF_ERROR(parse_exact_size_t(dimensions.substr(0, separator), "width").status());
    HM_RETURN_IF_ERROR(parse_exact_size_t(dimensions.substr(separator + 1), "height").status());
  }
  return absl::OkStatus();
}

absl::StatusOr<std::optional<CanvasSize>> stitched_output_size_from_generation(const std::string& output_generation) {
  constexpr std::string_view output_size_marker = "\noutput-size:";
  const size_t marker = output_generation.rfind(output_size_marker);
  if (marker == std::string::npos)
    return std::nullopt;
  if (output_generation.empty() || output_generation.back() != '\n')
    return absl::InvalidArgumentError("Invalid stitched-output dimensions");
  const size_t value_start = marker + output_size_marker.size();
  const std::string dimensions = output_generation.substr(value_start, output_generation.size() - value_start - 1);
  const size_t separator = dimensions.find('x');
  if (separator == std::string::npos || separator == 0 || separator + 1 >= dimensions.size())
    return absl::InvalidArgumentError("Invalid stitched-output dimensions");
  size_t width = 0;
  size_t height = 0;
  HM_ASSIGN_OR_RETURN(width, parse_exact_size_t(dimensions.substr(0, separator), "width"));
  HM_ASSIGN_OR_RETURN(height, parse_exact_size_t(dimensions.substr(separator + 1), "height"));
  return CanvasSize{.width = width, .height = height};
}

absl::StatusOr<std::string> stitched_output_generation_without_dimensions(const std::string& output_generation) {
  constexpr std::string_view output_size_marker = "\noutput-size:";
  const size_t marker = output_generation.rfind(output_size_marker);
  if (marker == std::string::npos)
    return output_generation;
  if (output_generation.empty() || output_generation.back() != '\n')
    return absl::InvalidArgumentError("Invalid stitched-output dimensions");
  return output_generation.substr(0, marker + 1);
}

absl::StatusOr<YAML::Node> load_config_or_empty(const fs::path& config_path) {
  try {
    if (fs::is_regular_file(config_path))
      return YAML::LoadFile(config_path.string());
    return YAML::Node(YAML::NodeType::Map);
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError("Unable to read rink profile YAML: " + std::string(exception.what()));
  }
}

} // namespace

absl::Status validate_stitched_output_generation_hugin(
    const std::string& output_generation,
    const std::string& expected_hugin_generation) {
  return validate_output_generation_hugin(output_generation, expected_hugin_generation);
}

absl::Status validate_stitched_output_generation_dimensions(
    const std::string& output_generation,
    size_t width,
    size_t height) {
  if (output_generation.empty() || width == 0 || height == 0)
    return absl::InvalidArgumentError("A stitched-output generation and positive image dimensions are required");
  std::optional<CanvasSize> expected_size;
  HM_ASSIGN_OR_RETURN(expected_size, stitched_output_size_from_generation(output_generation));
  if (!expected_size.has_value())
    return absl::FailedPreconditionError("Stitched-output generation is missing output dimensions");
  if (expected_size->width != width || expected_size->height != height) {
    return absl::AbortedError("Image dimensions do not match the stitched-output generation");
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> current_stitched_output_generation_id_locked(
    const std::string& game_dir,
    const YAML::Node& config,
    const std::string& hugin_generation) {
  if (game_dir.empty() || hugin_generation.empty())
    return absl::InvalidArgumentError("A game directory and Hugin generation are required");
  CanvasSize output_size;
  HM_ASSIGN_OR_RETURN(output_size, get_mapping_canvas_size(fs::path(game_dir)));
  std::optional<double> persisted_rotation_marker;
  HM_ASSIGN_OR_RETURN(persisted_rotation_marker, stitched_output_persisted_rotation_marker(config));
  if (persisted_rotation_marker.has_value()) {
    double persisted_rotation = 0.0;
    HM_ASSIGN_OR_RETURN(persisted_rotation, configured_post_stitch_rotation(config));
    if (persisted_rotation != *persisted_rotation_marker) {
      return absl::AbortedError(
          "Persisted stitched-output rotation changed after the current output generation was published");
    }
    try {
      const YAML::Node generation = config["rink"]["stitched_output_generation"];
      if (!generation || !generation.IsScalar()) {
        return absl::FailedPreconditionError(
            "Stitched-output persisted-rotation marker requires an authoritative producer generation");
      }
      const std::string authoritative_generation = generation.as<std::string>();
      HM_RETURN_IF_ERROR(validate_output_generation_hugin(authoritative_generation, hugin_generation));
      std::optional<CanvasSize> authoritative_size;
      HM_ASSIGN_OR_RETURN(authoritative_size, stitched_output_size_from_generation(authoritative_generation));
      if (!authoritative_size.has_value()) {
        return absl::FailedPreconditionError("Authoritative stitched-output generation is missing output dimensions");
      }
      if (authoritative_size->width != output_size.width || authoritative_size->height != output_size.height) {
        return absl::AbortedError(
            "Authoritative stitched-output generation does not match the current canvas dimensions");
      }
      return authoritative_generation;
    } catch (const YAML::Exception& exception) {
      return absl::InvalidArgumentError(
          "Unable to read authoritative stitched-output generation: " + std::string(exception.what()));
    }
  }
  return configured_output_generation(config, hugin_generation, output_size);
}

absl::Status validate_stitched_output_generation(
    const std::string& game_dir,
    const std::string& expected_output_generation,
    const std::string& expected_invalidation_id) {
  if (game_dir.empty() || expected_output_generation.empty())
    return absl::InvalidArgumentError("A game directory and stitched-output generation are required");

  const fs::path root(game_dir);
  auto hugin_lock = HuginProject::RecoverAndLock(root);
  if (!hugin_lock.ok())
    return hugin_lock.status();
  auto config_transaction = GameConfigTransactionLock::Acquire(root);
  if (!config_transaction.ok())
    return config_transaction.status();
  auto hugin_generation = HuginProject::GenerationId(root, **hugin_lock);
  if (!hugin_generation.ok())
    return hugin_generation.status();
  HM_RETURN_IF_ERROR(validate_output_generation_hugin(expected_output_generation, *hugin_generation));
  auto config = load_config_or_empty(root / "config.yaml");
  if (!config.ok())
    return config.status();
  return validate_stitching_generation_owner(*config, expected_invalidation_id);
}

absl::StatusOr<std::string> configured_stitched_output_generation_id(
    const std::string& game_dir,
    size_t max_output_width) {
  if (game_dir.empty())
    return absl::InvalidArgumentError("A game directory is required");
  const fs::path root(game_dir);
  auto hugin_lock = HuginProject::RecoverAndLock(root);
  if (!hugin_lock.ok())
    return hugin_lock.status();
  auto hugin_generation = HuginProject::GenerationId(root, **hugin_lock);
  if (!hugin_generation.ok())
    return hugin_generation.status();
  auto config_lock = GameConfigTransactionLock::Acquire(root);
  if (!config_lock.ok())
    return config_lock.status();
  auto config = load_config_or_empty(root / "config.yaml");
  if (!config.ok())
    return config.status();
  CanvasSize output_size;
  if (max_output_width > 0) {
    HM_ASSIGN_OR_RETURN(output_size, get_effective_mapping_canvas_size(root, max_output_width));
  } else {
    HM_ASSIGN_OR_RETURN(output_size, get_mapping_canvas_size(root));
  }
  return configured_output_generation(*config, *hugin_generation, output_size);
}

absl::StatusOr<std::string> current_stitched_output_generation_id(const std::string& game_dir) {
  if (game_dir.empty())
    return absl::InvalidArgumentError("A game directory is required");
  const fs::path root(game_dir);
  auto hugin_lock = HuginProject::RecoverAndLock(root);
  if (!hugin_lock.ok())
    return hugin_lock.status();
  auto config_transaction = GameConfigTransactionLock::Acquire(root);
  if (!config_transaction.ok())
    return config_transaction.status();
  auto hugin_generation = HuginProject::GenerationId(root, **hugin_lock);
  if (!hugin_generation.ok())
    return hugin_generation.status();
  auto config = load_config_or_empty(root / "config.yaml");
  if (!config.ok())
    return config.status();
  return current_stitched_output_generation_id_locked(game_dir, *config, *hugin_generation);
}

namespace {

using FieldMaskConsumer =
    std::function<absl::Status(const std::string& mask_path, const std::optional<CanvasSize>& expected_size)>;

absl::Status visit_current_field_mask_impl(
    const std::string& game_dir,
    const std::string& expected_output_generation,
    const std::string& expected_invalidation_id,
    const std::optional<size_t>& max_output_width,
    const std::optional<double>& post_stitch_rotate_degrees,
    const FieldMaskConsumer& consumer) {
  if (game_dir.empty()) {
    return absl::InvalidArgumentError("A game directory is required to load the field mask");
  }
  if (!consumer)
    return absl::InvalidArgumentError("A field-mask consumer is required");

  const fs::path root(game_dir);
  auto hugin_lock = HuginProject::RecoverAndLock(root);
  if (!hugin_lock.ok())
    return hugin_lock.status();
  auto config_transaction = GameConfigTransactionLock::Acquire(root);
  if (!config_transaction.ok())
    return config_transaction.status();
  auto hugin_generation = HuginProject::GenerationId(root, **hugin_lock);
  if (!hugin_generation.ok())
    return hugin_generation.status();
  auto config = load_config_or_empty(root / "config.yaml");
  if (!config.ok())
    return config.status();
  HM_RETURN_IF_ERROR(validate_stitching_generation_owner(*config, expected_invalidation_id));
  std::string current_output_generation;
  std::optional<std::string> compatible_legacy_generation;
  std::optional<CanvasSize> expected_canvas_size;
  if (!expected_output_generation.empty()) {
    HM_RETURN_IF_ERROR(validate_output_generation_hugin(expected_output_generation, *hugin_generation));
    current_output_generation = expected_output_generation;
    HM_ASSIGN_OR_RETURN(expected_canvas_size, stitched_output_size_from_generation(expected_output_generation));
    CanvasSize native_size;
    HM_ASSIGN_OR_RETURN(native_size, get_mapping_canvas_size(root));
    if (!expected_canvas_size.has_value()) {
      expected_canvas_size = native_size;
    } else if (expected_canvas_size->width == native_size.width && expected_canvas_size->height == native_size.height) {
      HM_ASSIGN_OR_RETURN(
          compatible_legacy_generation, stitched_output_generation_without_dimensions(expected_output_generation));
    }
  } else if (max_output_width.has_value()) {
    CanvasSize native_size;
    HM_ASSIGN_OR_RETURN(native_size, get_mapping_canvas_size(root));
    CanvasSize effective_size = native_size;
    if (*max_output_width > 0)
      HM_ASSIGN_OR_RETURN(effective_size, get_effective_mapping_canvas_size(root, *max_output_width));
    if (!post_stitch_rotate_degrees.has_value())
      return absl::InvalidArgumentError("An effective post-stitch rotation is required for stitching preflight");
    HM_ASSIGN_OR_RETURN(
        current_output_generation,
        stitched_output_generation_id(
            *hugin_generation, *post_stitch_rotate_degrees, effective_size.width, effective_size.height));
    expected_canvas_size = effective_size;
    if (effective_size.width == native_size.width && effective_size.height == native_size.height) {
      HM_ASSIGN_OR_RETURN(
          compatible_legacy_generation, stitched_output_generation_id(*hugin_generation, *post_stitch_rotate_degrees));
    }
  } else {
    std::optional<double> persisted_rotation_marker;
    HM_ASSIGN_OR_RETURN(persisted_rotation_marker, stitched_output_persisted_rotation_marker(*config));
    if (persisted_rotation_marker.has_value()) {
      HM_ASSIGN_OR_RETURN(
          current_output_generation,
          current_stitched_output_generation_id_locked(game_dir, *config, *hugin_generation));
    } else {
      HM_ASSIGN_OR_RETURN(current_output_generation, configured_output_generation(*config, *hugin_generation));
    }
    CanvasSize native_size;
    HM_ASSIGN_OR_RETURN(native_size, get_mapping_canvas_size(root));
    expected_canvas_size = native_size;
  }
  bool migrate_legacy_generation = false;
  try {
    const YAML::Node saved_generation = (*config)["rink"]["stitched_output_generation"];
    const bool current_matches = saved_generation && saved_generation.IsScalar() &&
        saved_generation.as<std::string>() == current_output_generation;
    const bool legacy_matches = compatible_legacy_generation.has_value() && saved_generation &&
        saved_generation.IsScalar() && saved_generation.as<std::string>() == *compatible_legacy_generation;
    if (!current_matches && !legacy_matches) {
      return absl::FailedPreconditionError("Field mask does not match the current stitched-output generation");
    }
    migrate_legacy_generation = legacy_matches;
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError("Invalid field-mask generation metadata: " + std::string(exception.what()));
  }
  const fs::path mask_path = root / "rink_mask_0.png";
  std::error_code ec;
  if (!fs::exists(mask_path, ec) || ec) {
    return absl::NotFoundError("Field mask is missing: " + mask_path.string());
  }
  const auto bytes = fs::file_size(mask_path, ec);
  if (ec || bytes == 0) {
    return absl::FailedPreconditionError("Field mask is empty or unreadable: " + mask_path.string());
  }

  const absl::Status consumed = consumer(mask_path.string(), expected_canvas_size);
  if (!consumed.ok())
    return consumed;
  if (migrate_legacy_generation) {
    (*config)["rink"]["stitched_output_generation"] = current_output_generation;
    return publish_game_config(root, YAML::Dump(*config) + "\n");
  }
  return absl::OkStatus();
}

absl::StatusOr<cv::Mat> load_field_mask_impl(
    const std::string& game_dir,
    const std::string& expected_output_generation,
    const std::string& expected_invalidation_id,
    const std::optional<size_t>& max_output_width,
    const std::optional<double>& post_stitch_rotate_degrees = std::nullopt) {
  cv::Mat mask;
  const absl::Status visit_status = visit_current_field_mask_impl(
      game_dir,
      expected_output_generation,
      expected_invalidation_id,
      max_output_width,
      post_stitch_rotate_degrees,
      [&](const std::string& mask_path, const std::optional<CanvasSize>& expected_canvas_size) {
        if (const char* delay = std::getenv("HM_TEST_FIELD_MASK_PRE_DECODE_DELAY_MS")) {
          const long delay_ms = std::strtol(delay, nullptr, 10);
          if (delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        mask = cv::imread(mask_path, cv::IMREAD_GRAYSCALE);
        if (mask.empty())
          return absl::InvalidArgumentError("Field mask could not be decoded: " + mask_path);
        if (const auto max_canvas_dimension = live_stitch_max_canvas_dimension(); max_canvas_dimension.has_value()) {
          const CanvasSize mask_size{
              .width = static_cast<size_t>(mask.cols),
              .height = static_cast<size_t>(mask.rows),
          };
          if (canvas_exceeds_max_dimension(mask_size, *max_canvas_dimension)) {
            std::cout << "Field mask canvas " << mask_size.width << "x" << mask_size.height
                      << " exceeds live-stitch max dimension " << *max_canvas_dimension << "; regenerating"
                      << std::endl;
            return absl::FailedPreconditionError("Field mask exceeds the live-stitch canvas limit");
          }
        }
        if (expected_canvas_size.has_value() &&
            (mask.cols != static_cast<int>(expected_canvas_size->width) ||
             mask.rows != static_cast<int>(expected_canvas_size->height))) {
          std::cout << "Field mask size " << mask.cols << "x" << mask.rows << " does not match stitched canvas "
                    << expected_canvas_size->width << "x" << expected_canvas_size->height << "; regenerating"
                    << std::endl;
          return absl::FailedPreconditionError("Field mask size does not match the stitched canvas");
        }
        return absl::OkStatus();
      });
  if (!visit_status.ok())
    return visit_status;
  return mask;
}

} // namespace

absl::Status visit_current_field_mask(
    const std::string& game_dir,
    const std::string& expected_output_generation,
    const std::string& expected_invalidation_id,
    const std::function<absl::Status(const std::string& mask_path)>& consumer) {
  if (!consumer)
    return absl::InvalidArgumentError("A field-mask consumer is required");
  return visit_current_field_mask_impl(
      game_dir,
      expected_output_generation,
      expected_invalidation_id,
      std::nullopt,
      std::nullopt,
      [&](const std::string& mask_path, const std::optional<CanvasSize>&) { return consumer(mask_path); });
}

absl::StatusOr<cv::Mat> load_field_mask(
    const std::string& game_dir,
    const std::string& expected_output_generation,
    const std::string& expected_invalidation_id) {
  return load_field_mask_impl(game_dir, expected_output_generation, expected_invalidation_id, std::nullopt);
}

bool is_field_mask_configured(
    const std::string& game_dir,
    const std::string& expected_output_generation,
    const std::string& expected_invalidation_id) {
  return load_field_mask(game_dir, expected_output_generation, expected_invalidation_id).ok();
}

bool is_field_mask_configured_for_stitching_config(
    const std::string& game_dir,
    size_t max_output_width,
    double post_stitch_rotate_degrees,
    const std::string& expected_invalidation_id) {
  return load_field_mask_impl(game_dir, {}, expected_invalidation_id, max_output_width, post_stitch_rotate_degrees)
      .ok();
}

namespace {

absl::Status validate_live_output_publication_authority(
    const YAML::Node& config,
    const std::string& expected_output_generation,
    const std::string& expected_output_authorization_id) {
  std::optional<std::string> pending_generation;
  std::optional<std::string> pending_authorization_id;
  if (config && config.IsDefined()) {
    if (!config.IsMap())
      return absl::InvalidArgumentError("Game config must be a map");
    const YAML::Node rink = config["rink"];
    if (rink && rink.IsDefined()) {
      if (!rink.IsMap())
        return absl::InvalidArgumentError("Rink config must be a map");
      const YAML::Node generation_node = rink["stitched_output_pending_generation"];
      if (generation_node && generation_node.IsDefined()) {
        if (!generation_node.IsScalar())
          return absl::InvalidArgumentError("Pending stitched-output generation must be a scalar");
        pending_generation = generation_node.as<std::string>();
      }
      const YAML::Node authorization_node = rink["stitched_output_pending_authorization_id"];
      if (authorization_node && authorization_node.IsDefined()) {
        if (!authorization_node.IsScalar())
          return absl::InvalidArgumentError("Pending stitched-output authorization ID must be a scalar");
        pending_authorization_id = authorization_node.as<std::string>();
      }
    }
  }
  if (pending_generation.has_value() != pending_authorization_id.has_value())
    return absl::InvalidArgumentError("Pending stitched-output authorization is incomplete");
  if (expected_output_generation.empty()) {
    if (!expected_output_authorization_id.empty())
      return absl::InvalidArgumentError("A live output authorization requires a stitched-output generation");
    return pending_generation.has_value()
        ? absl::AbortedError("A live stitched-output authorization owns artifact publication")
        : absl::OkStatus();
  }
  if (expected_output_authorization_id.empty()) {
    return pending_generation.has_value()
        ? absl::AbortedError("A live stitched-output authorization owns artifact publication")
        : absl::OkStatus();
  }
  if (!pending_generation.has_value() || *pending_generation != expected_output_generation ||
      *pending_authorization_id != expected_output_authorization_id) {
    return absl::AbortedError("A newer live stitched-output authorization owns artifact publication");
  }
  return absl::OkStatus();
}

} // namespace

absl::Status validate_field_mask_publication_authority(
    const std::string& game_dir,
    const std::string& expected_output_generation,
    const std::string& expected_output_authorization_id) {
  if (game_dir.empty() || expected_output_generation.empty())
    return absl::InvalidArgumentError("A game directory and stitched-output generation are required");
  const fs::path root(game_dir);
  auto config_lock = GameConfigLock::TryAcquire(root);
  if (!config_lock.ok())
    return config_lock.status();
  try {
    if (!fs::is_regular_file(root / "config.yaml"))
      return absl::NotFoundError("Game config is missing");
    const YAML::Node config = YAML::LoadFile((root / "config.yaml").string());
    return validate_live_output_publication_authority(
        config, expected_output_generation, expected_output_authorization_id);
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to validate field-mask publication authority: " + std::string(exception.what()));
  }
}

namespace {

bool scoreboard_polygon_is_disabled(const YAML::Node& polygon) {
  if (!polygon || !polygon.IsSequence() || polygon.size() != 4)
    return false;
  try {
    for (const YAML::Node& point : polygon) {
      if (!point.IsSequence() || point.size() != 2 || point[0].as<double>() != 0.0 || point[1].as<double>() != 0.0)
        return false;
    }
    return true;
  } catch (const YAML::Exception&) {
    return false;
  }
}

void remove_active_scoreboard_polygon(YAML::Node& config) {
  const YAML::Node polygon = config["rink"]["scoreboard"]["perspective_polygon"];
  if (polygon && polygon.IsDefined() && !scoreboard_polygon_is_disabled(polygon))
    config["rink"]["scoreboard"].remove("perspective_polygon");
}

} // namespace

absl::Status save_rink_profile_locked(
    const std::string& game_dir,
    const RinkProfile& profile,
    const std::string& hugin_generation,
    const std::string& expected_output_generation,
    const std::string& expected_output_authorization_id,
    const std::string& expected_invalidation_id,
    const std::optional<double>& expected_persisted_rotation,
    const cv::Mat* stitched_image = nullptr) {
  if (game_dir.empty() || profile.masks.empty()) {
    return absl::InvalidArgumentError("A game directory and at least one rink mask are required");
  }
  if (!std::isfinite(profile.centroid.x) || !std::isfinite(profile.centroid.y) ||
      !std::isfinite(profile.combined_bbox.x) || !std::isfinite(profile.combined_bbox.y) ||
      !std::isfinite(profile.combined_bbox.width) || !std::isfinite(profile.combined_bbox.height) ||
      profile.combined_bbox.width <= 0.0 || profile.combined_bbox.height <= 0.0 ||
      !std::all_of(profile.scores.begin(), profile.scores.end(), [](float score) {
        return std::isfinite(score) && score >= 0.0f && score <= 1.0f;
      })) {
    return absl::InvalidArgumentError("Rink profile geometry and scores must be finite and valid");
  }
  const cv::Size expected_size = profile.masks.front().size();
  for (const cv::Mat& mask : profile.masks) {
    if (mask.empty() || mask.type() != CV_8U || mask.size() != expected_size)
      return absl::InvalidArgumentError("Rink masks must be equally sized, non-empty CV_8U images");
  }
  if (stitched_image != nullptr && stitched_image->empty())
    return absl::InvalidArgumentError("A non-empty stitched calibration image is required");
  if (!expected_output_generation.empty()) {
    HM_RETURN_IF_ERROR(validate_stitched_output_generation_dimensions(
        expected_output_generation,
        static_cast<size_t>(expected_size.width),
        static_cast<size_t>(expected_size.height)));
    if (stitched_image != nullptr) {
      HM_RETURN_IF_ERROR(validate_stitched_output_generation_dimensions(
          expected_output_generation,
          static_cast<size_t>(stitched_image->cols),
          static_cast<size_t>(stitched_image->rows)));
    }
  }
  const fs::path root(game_dir);
  std::error_code error;
  auto config_transaction = GameConfigTransactionLock::Acquire(root);
  if (!config_transaction.ok())
    return config_transaction.status();

  std::string pattern = (root / ".hstream-rink-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  char* created = ::mkdtemp(writable.data());
  if (created == nullptr)
    return absl::InternalError("Unable to create rink profile staging directory");
  const fs::path staging(created);
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
  if (::chmod(staging.c_str(), 0700) != 0)
    return absl::InternalError("Unable to protect rink staging directory");

  for (size_t index = 0; index < profile.masks.size(); ++index) {
    const cv::Mat& mask = profile.masks[index];
    const fs::path path = staging / ("rink_mask_" + std::to_string(index) + ".png");
    if (!cv::imwrite(path.string(), mask))
      return absl::InternalError("Unable to stage rink mask " + path.string());
    const cv::Mat validation = cv::imread(path.string(), cv::IMREAD_GRAYSCALE);
    if (validation.empty() || validation.size() != expected_size) {
      return absl::InternalError("Staged rink mask failed validation: " + path.string());
    }
    auto sync_status = fsync_path(path);
    if (!sync_status.ok())
      return sync_status;
  }
  if (stitched_image != nullptr) {
    const fs::path path = staging / "s.png";
    if (!cv::imwrite(path.string(), *stitched_image))
      return absl::InternalError("Unable to stage stitched calibration image " + path.string());
    const cv::Mat validation = cv::imread(path.string(), cv::IMREAD_COLOR);
    if (validation.empty() || validation.size() != stitched_image->size())
      return absl::InternalError("Staged stitched calibration image failed validation: " + path.string());
    auto sync_status = fsync_path(path);
    if (!sync_status.ok())
      return sync_status;
  }

  const fs::path config_path = root / "config.yaml";
  YAML::Node config(YAML::NodeType::Map);
  try {
    if (fs::is_regular_file(config_path))
      config = YAML::LoadFile(config_path.string());
    HM_RETURN_IF_ERROR(validate_stitching_generation_owner(config, expected_invalidation_id));
    HM_RETURN_IF_ERROR(validate_live_output_publication_authority(
        config, expected_output_generation, expected_output_authorization_id));
    std::string current_output_generation;
    if (!expected_output_generation.empty()) {
      HM_RETURN_IF_ERROR(validate_output_generation_hugin(expected_output_generation, hugin_generation));
      if (!expected_persisted_rotation.has_value()) {
        return absl::InvalidArgumentError(
            "Expected stitched-output generation requires a persisted-rotation publication guard");
      }
      double current_persisted_rotation = 0.0;
      HM_ASSIGN_OR_RETURN(current_persisted_rotation, configured_post_stitch_rotation(config));
      if (current_persisted_rotation != *expected_persisted_rotation) {
        return absl::AbortedError("Cannot publish a rink profile after the persisted stitched-output rotation changed");
      }
      auto expected_size = stitched_output_size_from_generation(expected_output_generation);
      if (!expected_size.ok())
        return expected_size.status();
      if (!expected_size->has_value()) {
        return absl::InvalidArgumentError("Authoritative stitched-output generation must include output dimensions");
      }
      CanvasSize current_size;
      HM_ASSIGN_OR_RETURN(current_size, get_mapping_canvas_size(root));
      if (current_size.width != (*expected_size)->width || current_size.height != (*expected_size)->height) {
        return absl::AbortedError("Cannot publish a rink profile after the stitched-output canvas size changed");
      }
      std::string compatible_legacy_generation;
      HM_ASSIGN_OR_RETURN(
          compatible_legacy_generation, stitched_output_generation_without_dimensions(expected_output_generation));
      const YAML::Node saved_generation = config["rink"]["stitched_output_generation"];
      const bool has_pending_generation = config["rink"]["stitched_output_pending_generation"] &&
          config["rink"]["stitched_output_pending_generation"].IsDefined();
      if (saved_generation && saved_generation.IsDefined()) {
        if (!saved_generation.IsScalar())
          return absl::InvalidArgumentError("Persisted stitched-output generation must be a scalar");
        const bool generation_matches = saved_generation.as<std::string>() == expected_output_generation;
        const bool compatible_legacy_matches = saved_generation.as<std::string>() == compatible_legacy_generation;
        if (!generation_matches && !compatible_legacy_matches && !has_pending_generation &&
            !stitching_calibration_is_pending(config)) {
          return absl::AbortedError(
              "Cannot replace a completed stitched-output generation outside a pending calibration epoch");
        }
        if (!generation_matches && !compatible_legacy_matches)
          remove_active_scoreboard_polygon(config);
      }
      if (!saved_generation || !saved_generation.IsDefined())
        remove_active_scoreboard_polygon(config);
      // Runtime rotation may be an in-memory CLI/property override and is
      // therefore authoritative even when it intentionally differs from the
      // persisted YAML rotation guarded above.
      current_output_generation = expected_output_generation;
    } else {
      auto configured_generation = configured_output_generation(config, hugin_generation);
      if (!configured_generation.ok())
        return configured_generation.status();
      current_output_generation = *configured_generation;
    }
    config["rink"]["ice_contours_mask_count"] = profile.masks.size();
    config["rink"]["ice_contours_mask_centroid"] = std::vector<double>{profile.centroid.x, profile.centroid.y};
    config["rink"]["ice_contours_combined_bbox"] = std::vector<double>{
        profile.combined_bbox.x,
        profile.combined_bbox.y,
        profile.combined_bbox.x + profile.combined_bbox.width,
        profile.combined_bbox.y + profile.combined_bbox.height};
    config["rink"]["stitched_output_generation"] = current_output_generation;
    if (expected_output_generation.empty()) {
      config["rink"].remove("stitched_output_persisted_rotation_degrees");
      config["rink"].remove("stitched_output_pending_generation");
      config["rink"].remove("stitched_output_pending_authorization_id");
      config["rink"].remove("stitched_output_pending_previous_generation");
      config["rink"].remove("stitched_output_pending_previous_authorization_id");
      config["rink"].remove("stitched_output_pending_completed_scoreboard_polygon");
    } else {
      config["rink"]["stitched_output_persisted_rotation_degrees"] = *expected_persisted_rotation;
      if (!expected_output_authorization_id.empty()) {
        config["rink"].remove("stitched_output_pending_generation");
        config["rink"].remove("stitched_output_pending_authorization_id");
        config["rink"].remove("stitched_output_pending_previous_generation");
        config["rink"].remove("stitched_output_pending_previous_authorization_id");
        config["rink"].remove("stitched_output_pending_completed_scoreboard_polygon");
      }
    }
    if (!expected_invalidation_id.empty()) {
      YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
      calibration["status"] = "complete";
      calibration["rink_mask_status"] = "complete";
      calibration["invalidation_id"] = expected_invalidation_id;
      calibration.remove("stale_from");
      calibration.remove("artifacts_invalidated");
    }
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError("Unable to update rink profile YAML: " + std::string(exception.what()));
  }
  {
    std::ofstream output(staging / "config.yaml", std::ios::out | std::ios::trunc);
    if (!output)
      return absl::InternalError("Unable to stage rink profile config");
    output << config << '\n';
    output.flush();
    if (!output)
      return absl::InternalError("Unable to flush rink profile config");
  }
  if (::chmod((staging / "config.yaml").c_str(), 0600) != 0)
    return absl::InternalError("Unable to protect staged rink profile config: " + std::string(std::strerror(errno)));
  auto sync_status = fsync_path(staging / "config.yaml");
  if (!sync_status.ok())
    return sync_status;

  const fs::path previous = staging / "previous";
  fs::create_directory(previous, error);
  if (error)
    return absl::InternalError("Unable to create rink rollback directory: " + error.message());
  std::vector<fs::path> old_files;
  for (const auto& entry : fs::directory_iterator(root, error)) {
    if (error)
      return absl::InternalError("Unable to inspect old rink masks: " + error.message());
    const std::string name = entry.path().filename().string();
    if (is_rink_artifact_name(name) && (name != "s.png" || stitched_image != nullptr)) {
      old_files.push_back(entry.path());
    }
  }
  for (const fs::path& old : old_files) {
    auto preserve = link_rink_rollback_file(old, previous / old.filename());
    if (!preserve.ok())
      return preserve;
    sync_status = fsync_path(previous / old.filename());
    if (!sync_status.ok())
      return sync_status;
  }
  std::vector<fs::path> new_files;
  for (size_t index = 0; index < profile.masks.size(); ++index)
    new_files.push_back(staging / ("rink_mask_" + std::to_string(index) + ".png"));
  if (stitched_image != nullptr)
    new_files.push_back(staging / "s.png");
  new_files.push_back(staging / "config.yaml");
  std::set<std::string> published_names;
  for (const fs::path& old : old_files)
    published_names.insert(old.filename().string());
  for (const fs::path& source : new_files)
    published_names.insert(source.filename().string());
  std::ostringstream manifest;
  for (const std::string& name : published_names)
    manifest << name << '\n';
  sync_status = write_transaction_file(staging / "new-files", manifest.str());
  if (!sync_status.ok())
    return sync_status;
  sync_status = fsync_path(previous, true);
  if (!sync_status.ok())
    return sync_status;
  sync_status = fsync_path(staging, true);
  if (!sync_status.ok())
    return sync_status;
  sync_status = publish_transaction_state(staging, "PREPARED\n");
  if (!sync_status.ok())
    return sync_status;
  // Persist the PREPARED transaction directory entry before deleting or
  // replacing any flat config/mask artifacts in the game directory.
  sync_status = fsync_path(root, true);
  if (!sync_status.ok())
    return sync_status;
  cleanup.prepared = true;
  if (const char* interrupt = std::getenv("HM_TEST_RINK_INTERRUPT_AFTER_PREPARE_SYNC");
      interrupt != nullptr && std::string(interrupt) == "1") {
    return absl::InternalError("Injected rink interruption after durable preparation");
  }

  auto rollback_error = [&](const std::string& message) {
    const auto rollback_status = recover_rink_transactions_locked(root);
    if (!rollback_status.ok())
      return absl::InternalError(message + "; rollback also failed: " + std::string(rollback_status.message()));
    return absl::InternalError(message);
  };
  for (const std::string& name : published_names) {
    fs::remove(root / name, error);
    if (error)
      return rollback_error("Unable to remove old rink artifact: " + error.message());
  }
  for (const fs::path& source : new_files) {
    const fs::path destination = root / source.filename();
    fs::rename(source, destination, error);
    if (error)
      return rollback_error("Unable to publish rink artifact: " + error.message());
    sync_status = fsync_path(destination);
    if (!sync_status.ok())
      return rollback_error(std::string(sync_status.message()));
  }
  sync_status = fsync_path(root, true);
  if (!sync_status.ok())
    return rollback_error(std::string(sync_status.message()));
  sync_status = write_transaction_file(staging / "state.committed", "COMMITTED\n");
  if (!sync_status.ok())
    return rollback_error(std::string(sync_status.message()));
  fs::rename(staging / "state.committed", staging / "state", error);
  if (error)
    return rollback_error("Unable to commit rink transaction: " + error.message());
  sync_status = fsync_path(staging, true);
  if (!sync_status.ok())
    return sync_status;
  fs::remove_all(staging, error);
  if (error)
    return absl::InternalError("Unable to clean committed rink transaction: " + error.message());
  sync_status = fsync_path(root, true);
  if (!sync_status.ok())
    return sync_status;
  return absl::OkStatus();
}

absl::Status save_rink_profile(
    const std::string& game_dir,
    const RinkProfile& profile,
    const std::string& expected_invalidation_id) {
  if (game_dir.empty())
    return absl::InvalidArgumentError("A game directory is required");
  const fs::path root(game_dir);
  std::error_code error;
  fs::create_directories(root, error);
  if (error)
    return absl::InternalError("Unable to create rink profile directory: " + error.message());
  auto hugin_lock = HuginProject::RecoverAndLock(root);
  if (!hugin_lock.ok())
    return hugin_lock.status();
  auto hugin_generation = HuginProject::GenerationId(root, **hugin_lock);
  if (!hugin_generation.ok())
    return hugin_generation.status();
  return save_rink_profile_locked(game_dir, profile, *hugin_generation, {}, {}, expected_invalidation_id, std::nullopt);
}

absl::Status save_rink_profile_with_stitched_image(
    const std::string& game_dir,
    const RinkProfile& profile,
    const cv::Mat& stitched_image,
    const std::string& expected_invalidation_id,
    const std::string& expected_output_generation,
    const std::string& expected_output_authorization_id) {
  if (game_dir.empty())
    return absl::InvalidArgumentError("A game directory is required");
  const fs::path root(game_dir);
  std::error_code error;
  fs::create_directories(root, error);
  if (error)
    return absl::InternalError("Unable to create rink profile directory: " + error.message());
  auto hugin_lock = HuginProject::RecoverAndLock(root);
  if (!hugin_lock.ok())
    return hugin_lock.status();
  auto hugin_generation = HuginProject::GenerationId(root, **hugin_lock);
  if (!hugin_generation.ok())
    return hugin_generation.status();
  std::optional<double> expected_persisted_rotation;
  if (!expected_output_generation.empty()) {
    auto config_transaction = GameConfigTransactionLock::Acquire(root);
    if (!config_transaction.ok())
      return config_transaction.status();
    auto config = load_config_or_empty(root / "config.yaml");
    if (!config.ok())
      return config.status();
    HM_RETURN_IF_ERROR(validate_stitching_generation_owner(*config, expected_invalidation_id));
    HM_ASSIGN_OR_RETURN(expected_persisted_rotation, configured_post_stitch_rotation(*config));
  }
  if (const char* delay = std::getenv("HM_TEST_RINK_PRE_PUBLICATION_DELAY_MS")) {
    const long delay_ms = std::strtol(delay, nullptr, 10);
    if (delay_ms > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  }
  return save_rink_profile_locked(
      game_dir,
      profile,
      *hugin_generation,
      expected_output_generation,
      expected_output_authorization_id,
      expected_invalidation_id,
      expected_persisted_rotation,
      &stitched_image);
}

absl::Status create_field_mask(
    const std::string& game_dir,
    surface::Surface surface,
    const std::string& expected_output_generation,
    const std::string& expected_invalidation_id,
    const std::function<bool()>& is_cancelled,
    const std::string& expected_output_authorization_id) {
  if (is_cancelled && is_cancelled())
    return absl::CancelledError("Rink-mask calibration cancelled before inference");
  const fs::path root(game_dir);
  auto hugin_lock = HuginProject::RecoverAndLock(root);
  if (!hugin_lock.ok())
    return hugin_lock.status();
  auto hugin_generation = HuginProject::GenerationId(root, **hugin_lock);
  if (!hugin_generation.ok())
    return hugin_generation.status();
  if (!expected_output_generation.empty()) {
    HM_RETURN_IF_ERROR(validate_output_generation_hugin(expected_output_generation, *hugin_generation));
  }
  // Avoid GPU readback and rink inference after this calibration generation
  // has already been superseded. Publication validates again under the config
  // transaction lock because a newer invalidation can still arrive while the
  // expensive inference is running.
  std::optional<double> expected_persisted_rotation;
  if (!expected_invalidation_id.empty() || !expected_output_generation.empty()) {
    auto config_transaction = GameConfigTransactionLock::Acquire(root);
    if (!config_transaction.ok())
      return config_transaction.status();
    auto config = load_config_or_empty(root / "config.yaml");
    if (!config.ok())
      return config.status();
    HM_RETURN_IF_ERROR(validate_stitching_generation_owner(*config, expected_invalidation_id));
    HM_RETURN_IF_ERROR(validate_live_output_publication_authority(
        *config, expected_output_generation, expected_output_authorization_id));
    if (!expected_output_generation.empty())
      HM_ASSIGN_OR_RETURN(expected_persisted_rotation, configured_post_stitch_rotation(*config));
  }
  if (const char* delay = std::getenv("HM_TEST_RINK_INFERENCE_DELAY_MS")) {
    const long delay_ms = std::strtol(delay, nullptr, 10);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0L, delay_ms));
    while (std::chrono::steady_clock::now() < deadline) {
      if (is_cancelled && is_cancelled())
        return absl::CancelledError("Rink-mask calibration cancelled before inference");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  std::string pattern = (root / ".hstream-field-mask-input-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  char* created = ::mkdtemp(writable.data());
  if (created == nullptr)
    return absl::InternalError("Unable to create private rink input directory");
  const fs::path input_dir(created);
  struct RemoveInputDirectory {
    fs::path path;
    ~RemoveInputDirectory() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } input_cleanup{input_dir};
  if (::chmod(input_dir.c_str(), 0700) != 0)
    return absl::InternalError("Unable to protect private rink input directory");
  const fs::path stitched_path = input_dir / "s.png";
  HM_RETURN_IF_ERROR(save_image(surface, stitched_path));
  const cv::Mat stitched = cv::imread(stitched_path.string(), cv::IMREAD_COLOR);
  if (stitched.empty())
    return absl::FailedPreconditionError("Unable to reload stitched frame for rink inference");
  fs::path model_path;
  HM_ASSIGN_OR_RETURN(model_path, rink_model_path());
  std::unique_ptr<RinkSegmentation> model;
  HM_ASSIGN_OR_RETURN(model, RinkSegmentation::Create(model_path.string()));
  RinkProfile profile;
  HM_ASSIGN_OR_RETURN(profile, model->Infer(stitched, RinkSegmentation::kHockeyMomInferenceScale, is_cancelled));
  return save_rink_profile_locked(
      game_dir,
      profile,
      *hugin_generation,
      expected_output_generation,
      expected_output_authorization_id,
      expected_invalidation_id,
      expected_persisted_rotation,
      &stitched);
}

absl::Status configure_orientation(
    const std::string& game_dir,
    const std::string& expected_invalidation_id,
    const std::function<bool()>& is_cancelled) {
  fs::path model_path;
  HM_ASSIGN_OR_RETURN(model_path, rink_model_path());
  std::unique_ptr<RinkSegmentation> model;
  HM_ASSIGN_OR_RETURN(model, RinkSegmentation::Create(model_path.string()));
  return configure_game_orientation(game_dir, *model, expected_invalidation_id, is_cancelled);
}

bool is_scoreboard_configured(const std::string& game_dir) {
  const fs::path config_file = fs::path(game_dir) / "config.yaml";
  auto loaded_config = load_game_config_file(config_file);
  if (!loaded_config.ok() || !loaded_config->has_value()) {
    return false;
  }
  try {
    YAML::Node cfg = **loaded_config;
    const auto& rink = cfg["rink"];
    if (!rink || !rink.IsMap())
      return false;
    const auto& scoreboard = rink["scoreboard"];
    if (!scoreboard || !scoreboard.IsMap())
      return false;
    const auto& polygon = scoreboard["perspective_polygon"];
    return polygon && polygon.IsSequence() && polygon.size() == 4;
  } catch (...) {
    return false;
  }
}

absl::Status configure_scoreboard(const std::string& game_dir) {
  HM_RETURN_IF_ERROR(ScoreboardSelector::Run(game_dir));
  if (!is_scoreboard_configured(game_dir)) {
    return absl::InternalError("Scoreboard selector completed without writing perspective_polygon");
  }
  return absl::OkStatus();
}

absl::Status configure_stitching(
    const std::string& game_dir,
    surface::Surface left_surface,
    surface::Surface right_surface,
    const std::string& expected_invalidation_id,
    const std::function<bool()>& is_cancelled,
    size_t max_output_width) {
  return configure_stitching(
      game_dir,
      std::vector<StitchingCalibrationFramePair>{{left_surface, right_surface}},
      expected_invalidation_id,
      is_cancelled,
      max_output_width);
}

absl::Status configure_stitching(
    const std::string& game_dir,
    const std::vector<StitchingCalibrationFramePair>& frame_pairs,
    const std::string& expected_invalidation_id,
    const std::function<bool()>& is_cancelled,
    size_t max_output_width) {
  HM_RETURN_IF_ERROR(
      create_control_points(game_dir, frame_pairs, expected_invalidation_id, is_cancelled, max_output_width));
  return absl::OkStatus();
}

} // namespace stitching
} // namespace hm
