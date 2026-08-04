#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/CalibrationModels.h"
#include "hstream/src/libs/stitching/FeatureMatcher.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/Orientation.h"
#include "hstream/src/libs/stitching/RinkSegmentation.h"
#include "hstream/src/libs/stitching/ScoreboardSelector.h"
#include "hstream/src/libs/stitching/Synchronization.h"

#include <yaml-cpp/yaml.h>

#include "cupano/pano/cudaMat.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
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

void remove_cleanable_stitching_cache_keys(YAML::Node& config) {
  remove_yaml_key_path(config, {"stitching", "frame_offsets"});
  remove_yaml_key_path(config, {"game", "stitching", "frame_offsets"});
  remove_yaml_key_path(config, {"stitching", "control_points"});
  remove_yaml_key_path(config, {"game", "stitching", "control_points"});
  remove_yaml_key_path(config, {"rink", "scoreboard", "perspective_polygon"});
  remove_yaml_key_path(config, {"rink", "ice_contours_mask_count"});
  remove_yaml_key_path(config, {"rink", "ice_contours_mask_centroid"});
  remove_yaml_key_path(config, {"rink", "ice_contours_combined_bbox"});
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

  (void)TIFFGetField(tif, TIFFTAG_XPOSITION, &xpos);
  (void)TIFFGetField(tif, TIFFTAG_YPOSITION, &ypos);

  TIFFClose(tif);

  if (!have_dims || !width || !height) {
    return absl::InvalidArgumentError(TO_STRING("Missing TIFF dimensions: " << path.string()));
  }
  if (!have_res || xres <= 0.0f || yres <= 0.0f) {
    return absl::InvalidArgumentError(TO_STRING("Missing TIFF resolution: " << path.string()));
  }

  return TiffPlacement{
      .x_px = xpos * xres,
      .y_px = ypos * yres,
      .width = static_cast<int>(width),
      .height = static_cast<int>(height),
  };
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

  const float min_x = std::min(p0.x_px, p1.x_px);
  const float min_y = std::min(p0.y_px, p1.y_px);
  p0.x_px -= min_x;
  p1.x_px -= min_x;
  p0.y_px -= min_y;
  p1.y_px -= min_y;

  // Match hm::pano::ControlMasks::canvas_width/height semantics (float + int, then truncation).
  const int canvas_width = static_cast<int>(std::max(p0.x_px + p0.width, p1.x_px + p1.width));
  const int canvas_height = static_cast<int>(std::max(p0.y_px + p0.height, p1.y_px + p1.height));
  if (canvas_width <= 0 || canvas_height <= 0) {
    return absl::FailedPreconditionError(
        TO_STRING("Invalid canvas size computed from mapping TIFFs: " << canvas_width << "x" << canvas_height));
  }

  return CanvasSize{
      .width = static_cast<size_t>(canvas_width),
      .height = static_cast<size_t>(canvas_height),
  };
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
    // Runtime stitcher uses the per-camera mapping files; the panorama/seam preview artifacts are optional and may not
    // be generated in some environments (e.g. missing enblend deps). Keep the dependency check focused on required
    // runtime outputs so we can still run end-to-end.
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

absl::Status save_image(surface::Surface surf, const std::string& filename) {
  // CudaMat<typename T>
  if (surf.get_image_format() != IMAGE_RGBA8) {
    return absl::InvalidArgumentError("Invalid image format");
  }
  CudaMat<uchar4> gpu_image(
      SurfaceInfo{
          .width = (int)surf.width(),
          .height = (int)surf.height(),
          .pitch = (int)surf.pitch(),
          .data_ptr = surf.dataptr(),
      },
      /*B=*/1);
  cv::Mat cpu_img = gpu_image.download();
  if (cpu_img.empty()) {
    return absl::FailedPreconditionError("Unable to download image from GPU");
  }
  if (!cv::imwrite(filename, cpu_img)) {
    return absl::FailedPreconditionError("Unable to write image to file");
  }
  return absl::OkStatus();
}

} // namespace

absl::Status save_stitched_image(const std::string& game_dir, surface::Surface surface) {
  if (game_dir.empty()) {
    return absl::InvalidArgumentError("Cannot save stitched scoreboard image without a game directory");
  }
  std::error_code ec;
  fs::create_directories(game_dir, ec);
  if (ec) {
    return absl::InternalError(TO_STRING("Failed to create game directory \"" << game_dir << "\": " << ec.message()));
  }
  return save_image(surface, (fs::path(game_dir) / "s.png").string());
}

absl::Status clean_stitching_artifacts(const std::string& game_dir) {
  if (game_dir.empty()) {
    return absl::InvalidArgumentError("Missing game directory");
  }

  const fs::path game_dir_path(game_dir);
  std::error_code ec;
  if (!fs::exists(game_dir_path, ec) || ec) {
    return absl::NotFoundError(TO_STRING("Game directory does not exist: " << game_dir));
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
  HM_RETURN_IF_ERROR(clean_pattern("panorama.tif"));
  HM_RETURN_IF_ERROR(clean_pattern("seam_file.png"));
  HM_RETURN_IF_ERROR(clean_pattern("matches.png"));
  HM_RETURN_IF_ERROR(clean_pattern("keypoints.png"));
  HM_RETURN_IF_ERROR(clean_pattern("s.png"));
  HM_RETURN_IF_ERROR(clean_pattern("rink_mask_*.png"));
  size_t removed_extracted_frames = 0;
  HM_ASSIGN_OR_RETURN(removed_extracted_frames, delete_extracted_frames(game_dir_path));
  removed_files += removed_extracted_frames;

  const fs::path cfg_file_path = game_dir_path / "config.yaml";
  if (fs::exists(cfg_file_path)) {
    try {
      YAML::Node cfg = YAML::LoadFile(cfg_file_path.string());
      remove_cleanable_stitching_cache_keys(cfg);
      std::ofstream out(cfg_file_path, std::ios::out | std::ios::trunc);
      if (!out.is_open()) {
        return absl::InternalError(
            TO_STRING("Failed to open private config for writing: \"" << cfg_file_path.string() << '"'));
      }
      if (!is_empty_yaml_document(cfg)) {
        out << cfg << "\n";
      }
    } catch (const YAML::Exception& ex) {
      return absl::InternalError(
          TO_STRING("Failed to clean private config \"" << cfg_file_path.string() << "\": " << ex.what()));
    } catch (...) {
      return absl::InternalError(
          TO_STRING("Unknown error while cleaning private config \"" << cfg_file_path.string() << '"'));
    }
  }

  if (removed_files) {
    std::cout << "Removed " << removed_files << " stitch artifact file(s) from \"" << game_dir << "\"\n";
  }
  return absl::OkStatus();
}

absl::StatusOr<bool> is_stitching_configured(const std::string& game_dir) {
  bool up_to_date = test_dependency_tree(game_dir, /*add_rink_mask=*/false);
  if (!up_to_date) {
    return false;
  }
  const auto max_canvas_dimension = live_stitch_max_canvas_dimension();
  if (!max_canvas_dimension.has_value()) {
    return true;
  }
  auto canvas_size = get_mapping_canvas_size(fs::path(game_dir));
  if (!canvas_size.ok()) {
    std::cerr << "Warning: stitching artifacts exist but canvas size could not be read: " << canvas_size.status()
              << std::endl;
    return false;
  }
  if (canvas_exceeds_max_dimension(canvas_size.value(), *max_canvas_dimension)) {
    std::cout << "Stitching artifacts canvas " << canvas_size->width << "x" << canvas_size->height
              << " exceeds live-stitch max dimension " << *max_canvas_dimension << "; regenerating at a smaller scale"
              << std::endl;
    return false;
  }
  return true;
}

absl::StatusOr<bool> stitching_artifacts_exceed_live_canvas_limit(const std::string& game_dir) {
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
  return canvas_exceeds_max_dimension(canvas_size, *max_canvas_dimension);
}

absl::Status maybe_create_default_seam_file(const std::string& game_dir) {
  if (game_dir.empty()) {
    return absl::InvalidArgumentError("Game dir is empty");
  }

  const fs::path root = fs::path(game_dir);
  const fs::path seam_path = root / "seam_file.png";
  const bool seam_exists = fs::exists(seam_path);

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

  const float min_x = std::min(p0.x_px, p1.x_px);
  const float min_y = std::min(p0.y_px, p1.y_px);
  p0.x_px -= min_x;
  p1.x_px -= min_x;
  p0.y_px -= min_y;
  p1.y_px -= min_y;

  // Match hm::pano::ControlMasks::canvas_width/height semantics (float + int, then truncation).
  const int canvas_width = static_cast<int>(std::max(p0.x_px + p0.width, p1.x_px + p1.width));
  const int canvas_height = static_cast<int>(std::max(p0.y_px + p0.height, p1.y_px + p1.height));
  if (canvas_width <= 0 || canvas_height <= 0) {
    return absl::FailedPreconditionError(
        TO_STRING("Invalid canvas size computed from mapping TIFFs: " << canvas_width << "x" << canvas_height));
  }
  if (seam_exists) {
    cv::Mat existing = cv::imread(seam_path.string(), cv::IMREAD_UNCHANGED);
    if (!existing.empty() && existing.cols == canvas_width && existing.rows == canvas_height) {
      double min_value = 0.0;
      double max_value = 0.0;
      cv::minMaxLoc(existing, &min_value, &max_value);
      if (max_value > min_value) {
        return absl::OkStatus();
      }
      std::cerr << "Existing seam mask is uniform; regenerating " << seam_path.string() << std::endl;
    }
    if (existing.empty() || existing.cols != canvas_width || existing.rows != canvas_height) {
      std::cerr << "Existing seam mask does not match stitched canvas; regenerating " << seam_path.string() << " for "
                << canvas_width << "x" << canvas_height << std::endl;
    }
  }

  const int x0 = static_cast<int>(p0.x_px);
  const int y0 = static_cast<int>(p0.y_px);
  const int x1 = static_cast<int>(p1.x_px);
  const int y1 = static_cast<int>(p1.y_px);

  const int x0_end = x0 + p0.width;
  const int x1_end = x1 + p1.width;

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
  cv::Mat mask(canvas_height, canvas_width, CV_8U, cv::Scalar(0));
  if (seam_x < canvas_width) {
    mask.colRange(seam_x, canvas_width).setTo(255);
  }

  const int y0_end = y0 + p0.height;
  const int y1_end = y1 + p1.height;
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

  if (!cv::imwrite(seam_path.string(), mask)) {
    return absl::InternalError(TO_STRING("Failed to write seam mask: " << seam_path.string()));
  }

  std::cout << "Created fallback seam mask: " << seam_path.string() << " (" << canvas_width << "x" << canvas_height
            << ")" << std::endl;
  return absl::OkStatus();
}

bool can_configure_stitching(const YAML::Node& config) {
  return true;
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
    surface::Surface left_surface,
    surface::Surface right_surface) {
  fs::path left_file = fs::path(game_dir) / "left.png";
  fs::path right_file = fs::path(game_dir) / "right.png";
  HM_RETURN_IF_ERROR(save_image(left_surface, left_file));
  HM_RETURN_IF_ERROR(save_image(right_surface, right_file));

  size_t max_control_points = utils::getenv("HM_MAX_CONTROL_POINTS", kDefaultMaxControlPoints);
  const auto max_canvas_dimension = live_stitch_max_canvas_dimension();

  const cv::Mat left = cv::imread(left_file.string(), cv::IMREAD_COLOR);
  const cv::Mat right = cv::imread(right_file.string(), cv::IMREAD_COLOR);
  if (left.empty() || right.empty()) {
    return absl::FailedPreconditionError("Unable to reload synchronized frames for native feature matching");
  }
  fs::path model_path;
  HM_ASSIGN_OR_RETURN(model_path, feature_matcher_model_path());
  std::unique_ptr<FeatureMatcher> matcher;
  HM_ASSIGN_OR_RETURN(matcher, FeatureMatcher::Create(model_path.string()));
  FeatureMatchResult matched;
  HM_ASSIGN_OR_RETURN(matched, matcher->Infer(left, right, max_control_points));
  if (matched.accepted_match_count < 16) {
    return absl::FailedPreconditionError(TO_STRING(
        "Native feature matcher produced only " << matched.accepted_match_count
                                                << " usable matches; at least 16 are required"));
  }

  HuginProject::Options options;
  options.max_canvas_dimension = max_canvas_dimension;
  HM_RETURN_IF_ERROR(HuginProject::Configure(game_dir, matched.selected, options));
  return maybe_create_default_seam_file(game_dir);
}

namespace {

constexpr const char* kRinkTransactionPrefix = ".hmstream-rink-";

struct ScopedRinkLock {
  int descriptor{-1};
  ~ScopedRinkLock() {
    if (descriptor >= 0) {
      ::flock(descriptor, LOCK_UN);
      ::close(descriptor);
    }
  }
};

absl::StatusOr<std::unique_ptr<ScopedRinkLock>> lock_rink_transactions(const fs::path& root) {
  const fs::path path = root / ".hmstream-rink.lock";
  const int descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (descriptor < 0)
    return absl::InternalError("Unable to open rink transaction lock: " + std::string(std::strerror(errno)));
  if (::flock(descriptor, LOCK_EX) != 0) {
    const std::string message = std::strerror(errno);
    ::close(descriptor);
    return absl::InternalError("Unable to lock rink transaction: " + message);
  }
  auto lock = std::make_unique<ScopedRinkLock>();
  lock->descriptor = descriptor;
  return lock;
}

absl::Status fsync_path(const fs::path& path, bool directory = false) {
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
    std::ifstream state_input(transaction / "state");
    std::string state;
    std::getline(state_input, state);
    if (state == "PREPARED") {
      std::ifstream manifest(transaction / "new-files");
      std::string name;
      while (manifest >> name) {
        if (fs::path(name).filename() != name)
          return absl::InvalidArgumentError("Invalid rink transaction filename: " + name);
        fs::remove(root / name, error);
        if (error)
          return absl::InternalError("Unable to remove interrupted rink artifact: " + error.message());
      }
      const fs::path previous = transaction / "previous";
      if (fs::is_directory(previous, error)) {
        for (const auto& old : fs::directory_iterator(previous, error)) {
          if (error)
            return absl::InternalError("Unable to inspect rink transaction backup: " + error.message());
          fs::rename(old.path(), root / old.path().filename(), error);
          if (error)
            return absl::InternalError("Unable to restore interrupted rink artifact: " + error.message());
        }
      }
      error.clear();
      auto status = fsync_path(root, true);
      if (!status.ok())
        return status;
    }
    // COMMITTED transactions already have a durable new generation. A
    // directory with no state was interrupted before any root artifact moved.
    fs::remove_all(transaction, error);
    if (error)
      return absl::InternalError("Unable to clean rink transaction: " + error.message());
  }
  return fsync_path(root, true);
}

} // namespace

bool is_field_mask_configured(const std::string& game_dir) {
  if (game_dir.empty()) {
    return false;
  }

  const fs::path root(game_dir);
  auto lock = lock_rink_transactions(root);
  if (!lock.ok() || !recover_rink_transactions_locked(root).ok())
    return false;
  const fs::path mask_path = root / "rink_mask_0.png";
  std::error_code ec;
  if (!fs::exists(mask_path, ec) || ec) {
    return false;
  }
  const auto bytes = fs::file_size(mask_path, ec);
  if (ec || bytes == 0) {
    return false;
  }
  cv::Mat mask = cv::imread(mask_path.string(), cv::IMREAD_UNCHANGED);
  if (mask.empty()) {
    return false;
  }
  if (const auto max_canvas_dimension = live_stitch_max_canvas_dimension(); max_canvas_dimension.has_value()) {
    const CanvasSize mask_size{
        .width = static_cast<size_t>(mask.cols),
        .height = static_cast<size_t>(mask.rows),
    };
    if (canvas_exceeds_max_dimension(mask_size, *max_canvas_dimension)) {
      std::cout << "Field mask canvas " << mask_size.width << "x" << mask_size.height
                << " exceeds live-stitch max dimension " << *max_canvas_dimension << "; regenerating" << std::endl;
      return false;
    }
  }
  auto canvas_size = get_mapping_canvas_size(fs::path(game_dir));
  if (canvas_size.ok()) {
    if (mask.cols != static_cast<int>(canvas_size->width) || mask.rows != static_cast<int>(canvas_size->height)) {
      std::cout << "Field mask size " << mask.cols << "x" << mask.rows << " does not match stitched canvas "
                << canvas_size->width << "x" << canvas_size->height << "; regenerating" << std::endl;
      return false;
    }
  }
  return true;
}

absl::Status save_rink_profile(const std::string& game_dir, const RinkProfile& profile) {
  if (game_dir.empty() || profile.masks.empty()) {
    return absl::InvalidArgumentError("A game directory and at least one rink mask are required");
  }
  const fs::path root(game_dir);
  std::error_code error;
  fs::create_directories(root, error);
  if (error)
    return absl::InternalError("Unable to create rink profile directory: " + error.message());
  auto transaction_lock = lock_rink_transactions(root);
  if (!transaction_lock.ok())
    return transaction_lock.status();
  auto recovery = recover_rink_transactions_locked(root);
  if (!recovery.ok())
    return recovery;

  std::string pattern = (root / ".hmstream-rink-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  char* created = ::mkdtemp(writable.data());
  if (created == nullptr)
    return absl::InternalError("Unable to create rink profile staging directory");
  const fs::path staging(created);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{staging};
  if (::chmod(staging.c_str(), 0700) != 0)
    return absl::InternalError("Unable to protect rink staging directory");

  const cv::Size expected_size = profile.masks.front().size();
  for (size_t index = 0; index < profile.masks.size(); ++index) {
    const cv::Mat& mask = profile.masks[index];
    if (mask.empty() || mask.type() != CV_8U || mask.size() != expected_size) {
      return absl::InvalidArgumentError("Rink masks must be equally sized, non-empty CV_8U images");
    }
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

  const fs::path config_path = root / "config.yaml";
  YAML::Node config(YAML::NodeType::Map);
  try {
    if (fs::is_regular_file(config_path))
      config = YAML::LoadFile(config_path.string());
    config["rink"]["ice_contours_mask_count"] = profile.masks.size();
    config["rink"]["ice_contours_mask_centroid"] = std::vector<double>{profile.centroid.x, profile.centroid.y};
    config["rink"]["ice_contours_combined_bbox"] = std::vector<double>{
        profile.combined_bbox.x,
        profile.combined_bbox.y,
        profile.combined_bbox.x + profile.combined_bbox.width,
        profile.combined_bbox.y + profile.combined_bbox.height};
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
    if ((name.rfind("rink_mask_", 0) == 0 && entry.path().extension() == ".png") || name == "config.yaml") {
      old_files.push_back(entry.path());
    }
  }
  for (const fs::path& old : old_files) {
    fs::copy_file(old, previous / old.filename(), fs::copy_options::overwrite_existing, error);
    if (error)
      return absl::InternalError("Unable to preserve old rink artifact: " + error.message());
    sync_status = fsync_path(previous / old.filename());
    if (!sync_status.ok())
      return sync_status;
  }
  std::vector<fs::path> new_files;
  for (size_t index = 0; index < profile.masks.size(); ++index)
    new_files.push_back(staging / ("rink_mask_" + std::to_string(index) + ".png"));
  new_files.push_back(staging / "config.yaml");
  std::ostringstream manifest;
  for (const fs::path& source : new_files)
    manifest << source.filename().string() << '\n';
  sync_status = write_transaction_file(staging / "new-files", manifest.str());
  if (!sync_status.ok())
    return sync_status;
  sync_status = fsync_path(previous, true);
  if (!sync_status.ok())
    return sync_status;
  sync_status = fsync_path(staging, true);
  if (!sync_status.ok())
    return sync_status;
  sync_status = write_transaction_file(staging / "state", "PREPARED\n");
  if (!sync_status.ok())
    return sync_status;
  sync_status = fsync_path(staging, true);
  if (!sync_status.ok())
    return sync_status;

  auto rollback_error = [&](const std::string& message) {
    const auto rollback_status = recover_rink_transactions_locked(root);
    if (!rollback_status.ok())
      return absl::InternalError(message + "; rollback also failed: " + std::string(rollback_status.message()));
    return absl::InternalError(message);
  };
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

absl::Status create_field_mask(const std::string& game_dir, surface::Surface surface) {
  HM_RETURN_IF_ERROR(save_stitched_image(game_dir, surface));
  const cv::Mat stitched = cv::imread((fs::path(game_dir) / "s.png").string(), cv::IMREAD_COLOR);
  if (stitched.empty())
    return absl::FailedPreconditionError("Unable to reload stitched frame for rink inference");
  fs::path model_path;
  HM_ASSIGN_OR_RETURN(model_path, rink_model_path());
  std::unique_ptr<RinkSegmentation> model;
  HM_ASSIGN_OR_RETURN(model, RinkSegmentation::Create(model_path.string()));
  RinkProfile profile;
  HM_ASSIGN_OR_RETURN(profile, model->Infer(stitched, RinkSegmentation::kHockeyMomInferenceScale));
  return save_rink_profile(game_dir, profile);
}

absl::Status configure_orientation(const std::string& game_dir) {
  fs::path model_path;
  HM_ASSIGN_OR_RETURN(model_path, rink_model_path());
  std::unique_ptr<RinkSegmentation> model;
  HM_ASSIGN_OR_RETURN(model, RinkSegmentation::Create(model_path.string()));
  return configure_game_orientation(game_dir, *model);
}

bool is_scoreboard_configured(const std::string& game_dir) {
  const fs::path config_file = fs::path(game_dir) / "config.yaml";
  if (!fs::exists(config_file)) {
    return false;
  }
  try {
    YAML::Node cfg = YAML::LoadFile(config_file.string());
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
    surface::Surface right_surface) {
  HM_RETURN_IF_ERROR(create_control_points(game_dir, left_surface, right_surface));
  return absl::OkStatus();
}

} // namespace stitching
} // namespace hm
