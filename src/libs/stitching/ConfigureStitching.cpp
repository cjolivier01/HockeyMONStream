#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/common/Process.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/Synchronization.h"

#include "cupano/pano/cudaMat.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include <opencv2/opencv.hpp>
#include <tiffio.h>
#include "absl/strings/str_split.h"

namespace fs = std::filesystem;

extern "C" char** environ;

namespace hm {
namespace stitching {

namespace {

const std::string lfo_prefix = "Left frame offset: ";
const std::string rfo_prefix = "Right frame offset: ";

struct TiffPlacement {
  float x_px{0.0f};
  float y_px{0.0f};
  int width{0};
  int height{0};
};

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
  const bool have_res =
      TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres) && TIFFGetField(tif, TIFFTAG_YRESOLUTION, &yres);

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

// -----------------------------------------------------------------------------
// FileNode: Represents one file and its dependency children.
// A file that depends on multiple parents can simply be listed as a child
// under each parent.
// -----------------------------------------------------------------------------
struct FileNode {
  std::string filename;
  std::vector<FileNode> children;
};

std::string to_command_line(const std::vector<std::string>& cmd) {
  std::stringstream ss;
  for (size_t i = 0, n = cmd.size(); i < n; ++i) {
    if (i) {
      ss << ' ';
    }
    ss << cmd[i];
  }
  return ss.str();
}

// -----------------------------------------------------------------------------
// ValidationResult: Returned by the checker function.
//   - valid: true if every child is newer than its parent for every dependency edge.
//   - levels: a list of tree depths (starting at 0 for the root) where at least one violation occurred.
// -----------------------------------------------------------------------------
struct ValidationResult {
  bool valid;
  std::vector<int> levels;
};

fs::file_time_type getMostRecentWriteTime(const std::vector<std::string>& filenames) {
  if (filenames.empty()) {
    throw std::invalid_argument("No filenames provided.");
  }

  bool foundValidFile = false;
  fs::file_time_type latest;

  for (const auto& filename : filenames) {
    fs::path filePath(filename);
    if (fs::exists(filePath)) {
      try {
        fs::file_time_type ftime = fs::last_write_time(filePath);
        if (!foundValidFile) {
          latest = ftime;
          foundValidFile = true;
        } else if (ftime > latest) {
          latest = ftime;
        }
      } catch (const fs::filesystem_error& e) {
        std::cerr << "Error retrieving last write time for " << filename << ": " << e.what() << std::endl;
      }
    } else {
      std::cerr << "File not found: " << filename << std::endl;
    }
  }

  if (!foundValidFile) {
    throw std::runtime_error("No valid files found to determine write time.");
  }

  return latest;
}

fs::file_time_type getOldestWriteTime(const std::vector<std::string>& filenames) {
  if (filenames.empty()) {
    throw std::invalid_argument("No filenames provided.");
  }

  bool foundValidFile = false;
  fs::file_time_type oldest;

  for (const auto& filename : filenames) {
    fs::path filePath(filename);
    if (fs::exists(filePath)) {
      try {
        fs::file_time_type ftime = fs::last_write_time(filePath);
        if (!foundValidFile) {
          oldest = ftime;
          foundValidFile = true;
        } else if (ftime < oldest) {
          oldest = ftime;
        }
      } catch (const fs::filesystem_error& e) {
        std::cerr << "Error retrieving last write time for " << filename << ": " << e.what() << std::endl;
      }
    } else {
      std::cerr << "File not found: " << filename << std::endl;
    }
  }

  if (!foundValidFile) {
    throw std::runtime_error("No valid files found to determine write time.");
  }

  return oldest;
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
ValidationResult checkFileDependencies(const fs::path& dir_name, const FileNode& node, int level = 0) {
  ValidationResult result{true, {}};

  std::vector<std::string> filenames = absl::StrSplit(node.filename, ',');
  if (!dir_name.empty()) {
    for (auto& s : filenames) {
      s = dir_name / s;
    }
  }

  if (!std::all_of(filenames.begin(), filenames.end(), [](const std::string& f) { return fs::exists(f); })) {
    result.valid = false;
    result.levels.push_back(level);
    return result;
  }

  // Retrieve parent's modification time.
  std::error_code ec;
  auto parentTime = getMostRecentWriteTime(filenames);

  // Process each child dependency edge.
  for (const auto& child : node.children) {
    // Get the child's modification time.
    std::vector<std::string> child_names = absl::StrSplit(child.filename, ',');
    if (!dir_name.empty()) {
      for (auto& s : child_names) {
        s = dir_name / s;
      }
    }

    if (!std::all_of(child_names.begin(), child_names.end(), [](const std::string& f) { return fs::exists(f); })) {
      result.valid = false;
      result.levels.push_back(level + 1);
      return result;
    }

    auto childTime = getOldestWriteTime(child_names);
    if (childTime < parentTime) {
      // Violation: child file is not newer than the parent.
      std::cerr << "Violation: \"" << child.filename << "\" (time: " << childTime.time_since_epoch().count()
                << ") is older than its parent \"" << node.filename
                << "\" (time: " << parentTime.time_since_epoch().count() << ").\n";
      result.valid = false;
      result.levels.push_back(level + 1);
    }
    // Recursively check the child's dependency subtree.
    ValidationResult childResult = checkFileDependencies(dir_name, child, level + 1);
    if (!childResult.valid) {
      result.valid = false;
      // Append any levels from the child.
      result.levels.insert(result.levels.end(), childResult.levels.begin(), childResult.levels.end());
    }
  }
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

std::string get_game_id(const std::string& game_path) {
  fs::path path(game_path);
  if (path.empty()) {
    return std::string{};
  }

  fs::path normalized = path.lexically_normal();
  std::error_code ec;

  // If the path exists, prefer the filesystem type over heuristics.
  if (fs::exists(normalized, ec) && !ec) {
    if (fs::is_directory(normalized, ec) && !ec) {
      std::string leaf = normalized.filename().string();
      if (!leaf.empty()) {
        return leaf;
      }
      return normalized.parent_path().filename().string();
    }
    return normalized.parent_path().filename().string();
  }

  // Fallback when path doesn't exist yet: infer file vs directory shape.
  std::string leaf = normalized.filename().string();
  if (leaf.empty()) {
    return normalized.parent_path().filename().string();
  }
  if (normalized.has_extension()) {
    return normalized.parent_path().filename().string();
  }
  return leaf;
}

std::map<std::string, std::string> get_environment() {
  std::map<std::string, std::string> env_vars;
  // extern char** environ is a global variable containing the environment variables

  // Loop through the environment variables
  for (char** env = environ; *env != nullptr; env++) {
    std::string envEntry = *env;
    size_t pos = envEntry.find('=');
    if (pos != std::string::npos) {
      std::string key = envEntry.substr(0, pos);
      std::string value = envEntry.substr(pos + 1);
      env_vars[key] = value;
    }
  }
  return env_vars;
}

std::optional<fs::path> find_executable_maybe_conda(const std::string& exec) {
  auto found_exec = findExecutable(exec, {"PATH"});
  if (found_exec) {
    return *found_exec;
  }
  const char* s = getenv("CONDA_PREFIX");
  if (!s) {
    return std::nullopt;
  }
  auto path = fs::path(s) / "bin" / exec;
  if (!fs::exists(path)) {
    return std::nullopt;
  }
  return path;
}

std::string get_python_interp() {
  auto python_exec = findExecutable("python3", {"PATH"});
  if (!python_exec) {
    return "/usr/bin/python3";
  }
  return *python_exec;
}

std::optional<std::string> resolve_executable(const std::string& executable) {
  if (executable.empty()) {
    return std::nullopt;
  }
  if (executable[0] == '/' || executable[0] == '\\') {
    if (!std::filesystem::exists(executable)) {
      return std::nullopt;
    }
    return executable;
  }
  return findExecutable(executable, {"PATH"});
}

std::map<std::string, std::string> python_env(const std::string& add_dir, std::map<std::string, std::string> prev) {
  if (!add_dir.empty()) {
    std::string pythonpath;
    auto it = prev.find("PYTHONPATH");
    if (it != prev.end()) {
      pythonpath = it->second;
    }
    if (pythonpath.empty()) {
      pythonpath = add_dir;
    } else {
      pythonpath = add_dir + ':' + pythonpath;
    }
    prev["PYTHONPATH"] = pythonpath;
  }

  // Match hm_run.sh behavior: ensure conda libs are visible to python tools.
  const char* conda_prefix = ::getenv("CONDA_PREFIX");
  if (conda_prefix && *conda_prefix) {
    const std::string conda_lib = (fs::path(conda_prefix) / "lib").string();
    std::string ld = prev["LD_LIBRARY_PATH"];
    if (!conda_lib.empty() && ld.find(conda_lib) == std::string::npos) {
      ld = conda_lib + (ld.empty() ? "" : ":") + ld;
      prev["LD_LIBRARY_PATH"] = ld;
    }
  }
  return prev;
}

bool is_hmlib_repo_root(const fs::path& p) {
  std::error_code ec;
  return fs::is_directory(p / "hmlib", ec);
}

std::optional<fs::path> find_hmlib_repo_root() {
  if (const char* s = ::getenv("HMLIB_ROOT"); s && *s && is_hmlib_repo_root(s)) {
    return fs::path(s);
  }
  if (const char* s = ::getenv("HM_ROOT"); s && *s && is_hmlib_repo_root(s)) {
    return fs::path(s);
  }

  const fs::path cwd = fs::current_path();
  const std::vector<fs::path> candidates = {
      cwd / ".." / "hm",
      cwd / "bazel-hstream" / "external" / "hm",
      cwd / "external" / "hm",
  };
  for (const auto& cand : candidates) {
    if (is_hmlib_repo_root(cand)) {
      return cand;
    }
  }
  return std::nullopt;
}

std::map<std::string, std::string> hmlib_python_env(std::map<std::string, std::string> prev) {
  const auto hm_root = find_hmlib_repo_root();
  if (!hm_root.has_value()) {
    return python_env("", std::move(prev));
  }
  std::string pythonpath_prefix = hm_root->string();
  if (fs::is_directory(*hm_root / "src")) {
    pythonpath_prefix += ':' + (fs::path(*hm_root) / "src").string();
  }
  return python_env(pythonpath_prefix, std::move(prev));
}

} // namespace

absl::StatusOr<bool> is_stitching_configured(const std::string& game_dir) {
  bool up_to_date = test_dependency_tree(game_dir, /*add_rink_mask=*/false);
  if (up_to_date) {
    return true;
  }
  return false;
}

absl::Status maybe_create_default_seam_file(const std::string& game_dir) {
  if (game_dir.empty()) {
    return absl::InvalidArgumentError("Game dir is empty");
  }

  const fs::path root = fs::path(game_dir);
  const fs::path seam_path = root / "seam_file.png";
  if (fs::exists(seam_path)) {
    return absl::OkStatus();
  }

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
    return absl::FailedPreconditionError(TO_STRING(
        "Invalid canvas size computed from mapping TIFFs: " << canvas_width << "x" << canvas_height));
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
#if 1
  auto frame_offsets = synchronize_by_audio(video1, video2);
  return Synchronization{
      .video1_frame_offset = frame_offsets.first,
      .video2_frame_offset = frame_offsets.second,
  };
#else
  fs::path hm_cupano_dir = fs::path("external") / "hm-cupano";
  std::vector<std::string> cmd{
      "/home/colivier/miniforge3/envs/ubuntu/bin/python",
      fs::path("scripts") / "create_control_points.py",
      "--synchronize-only",
      "--left",
      video1,
      "--right",
      video2,
  };
  std::optional<double> v1_offset, v2_offset;
  int exitcode = run_command(
      cmd,
      hm_cupano_dir,
      get_environment(),
      [&v1_offset, &v2_offset](const std::string& stderr, const std::string& stdout) -> void {
        if (!stderr.empty()) {
          std::cerr << stderr << std::endl;
        }
        if (!stdout.empty()) {
          if (!strncmp(stdout.c_str(), lfo_prefix.c_str(), lfo_prefix.size())) {
            char* endptr = (char*)stdout.c_str() + stdout.size();
            v1_offset = std::strtod(stdout.c_str() + lfo_prefix.size(), &endptr);
            std::cout << stdout << std::endl;
          }
          if (!strncmp(stdout.c_str(), rfo_prefix.c_str(), rfo_prefix.size())) {
            char* endptr = (char*)stdout.c_str() + stdout.size();
            v2_offset = ::strtod(stdout.c_str() + rfo_prefix.size(), &endptr);
            std::cout << stdout << std::endl;
          }
        }
      });
  if (exitcode) {
    return absl::InternalError("Failed to create control points");
  }
  if (!v1_offset.has_value() || !v2_offset.has_value()) {
    return absl::InternalError("Failed to parse frame offsets from output");
  }
  return Synchronization{
      .video1_frame_offset = *v1_offset,
      .video2_frame_offset = *v2_offset,
  };
#endif
}

absl::StatusOr<std::string> find_and_validate_executable(
    const std::string& executable_name,
    const std::string& package = "hmlib") {
  auto hmcreate_control_points = resolve_executable(executable_name);
  if (!hmcreate_control_points.has_value()) {
    return absl::NotFoundError(TO_STRING(
        "Could not find executable: \"" << executable_name << "\", did you forget to install the \"" << package
                                        << "\" package?"));
  }
  return *hmcreate_control_points;
}

absl::Status create_control_points(
    const std::string& game_dir,
    surface::Surface left_surface,
    surface::Surface right_surface) {
  fs::path left_file = fs::path(game_dir) / "left.png";
  fs::path right_file = fs::path(game_dir) / "right.png";
  HM_RETURN_IF_ERROR(save_image(left_surface, left_file));
  HM_RETURN_IF_ERROR(save_image(right_surface, right_file));

  size_t max_control_points = utils::getenv("HM_MAX_CONTROL_POINTS", 500UL);

  std::vector<std::string> cmd;
  std::string working_dir;
  auto env = get_environment();

  const auto hmcreate_control_points = resolve_executable("hmcreate_control_points");
  if (hmcreate_control_points.has_value()) {
    cmd = {
        *hmcreate_control_points,
        "--left",
        left_file,
        "--right",
        right_file,
        TO_STRING("--max-control-points=" << max_control_points),
#ifdef __aarch64__
        "--scale=0.6",
#endif
    };
  } else {
    const auto python_exec = find_executable_maybe_conda("python3");
    if (!python_exec.has_value()) {
      return absl::NotFoundError("Could not find python3 for hmcreate_control_points fallback");
    }
    cmd = {
        python_exec->string(),
        "-m",
        "hmlib.cli.create_control_points",
        "--left",
        left_file,
        "--right",
        right_file,
        TO_STRING("--max-control-points=" << max_control_points),
#ifdef __aarch64__
        "--scale=0.6",
#endif
    };
    env = hmlib_python_env(std::move(env));
    if (auto hm_root = find_hmlib_repo_root(); hm_root.has_value()) {
      working_dir = hm_root->string();
    }
  }

  int exitcode = run_command(cmd, working_dir, env, [](const std::string& stderr, const std::string& stdout) -> void {
    if (!stderr.empty()) {
      std::cerr << stderr << std::endl;
    }
    if (!stdout.empty()) {
      std::cerr << stdout << std::endl;
    }
  });
  if (exitcode) {
    return absl::InternalError(TO_STRING("Failed to create control points: " << to_command_line(cmd)));
  }

  return absl::OkStatus();
}

bool is_field_mask_configured(const std::string& game_dir) {
  if (game_dir.empty()) {
    return false;
  }

  const fs::path mask_path = fs::path(game_dir) / "rink_mask_0.png";
  std::error_code ec;
  if (!fs::exists(mask_path, ec) || ec) {
    return false;
  }
  const auto bytes = fs::file_size(mask_path, ec);
  if (ec || bytes == 0) {
    return false;
  }
  return true;
}

absl::Status create_field_mask(const std::string& game_dir, surface::Surface surface) {
  fs::path stitched_file = fs::path(game_dir) / "s.png";
  HM_RETURN_IF_ERROR(save_image(surface, stitched_file));
  std::string game_id = get_game_id(stitched_file);

  std::vector<std::string> cmd;
  std::string working_dir;
  auto env = hmlib_python_env(get_environment());

  const auto hmfind_ice_rink = resolve_executable("hmfind_ice_rink");
  if (hmfind_ice_rink.has_value()) {
    cmd = {
        *hmfind_ice_rink,
        "--game-id",
        game_id,
#ifdef __aarch64__
        "--device=cuda",
#else
        "--device=cpu",
#endif
    };
  } else {
    const auto python_exec = find_executable_maybe_conda("python3");
    if (!python_exec.has_value()) {
      return absl::NotFoundError("Could not find python3 for hmfind_ice_rink fallback");
    }
    cmd = {
        python_exec->string(),
        "-m",
        "hmlib.cli.find_ice_rink",
        "--game-id",
        game_id,
#ifdef __aarch64__
        "--device=cuda",
#else
        "--device=cpu",
#endif
    };
    if (auto hm_root = find_hmlib_repo_root(); hm_root.has_value()) {
      working_dir = hm_root->string();
    }
  }

  int exitcode =
      run_command(cmd, working_dir, env, [](const std::string& stderr, const std::string& stdout) -> void {
        if (!stderr.empty()) {
          std::cerr << stderr << std::endl;
        }
        if (!stdout.empty()) {
          std::cerr << stdout << std::endl;
        }
      });
  if (exitcode) {
    return absl::InternalError("Failed to create control points");
  }

  return absl::OkStatus();
}

absl::Status configure_orientation(const std::string& game_dir) {
  std::string game_id = get_game_id(game_dir);

  std::vector<std::string> cmd;
  std::string working_dir;
  auto env = hmlib_python_env(get_environment());

  std::optional<fs::path> exec = find_executable_maybe_conda("hmorientation");
  if (exec.has_value()) {
    cmd = {
        exec->string(),
        "--game-id",
        game_id,
    };
  } else {
    const auto python_exec = find_executable_maybe_conda("python3");
    if (!python_exec.has_value()) {
      return absl::NotFoundError("Could not find python3 for hmorientation fallback");
    }
    cmd = {
        python_exec->string(),
        "-m",
        "hmlib.orientation",
        "--game-id",
        game_id,
    };
    if (auto hm_root = find_hmlib_repo_root(); hm_root.has_value()) {
      working_dir = hm_root->string();
    }
  }

  int exitcode = run_command(cmd, working_dir, env, [](const std::string& stderr, const std::string& stdout) -> void {
    if (!stderr.empty()) {
      std::cerr << stderr << std::endl;
    }
    if (!stdout.empty()) {
      std::cerr << stdout << std::endl;
    }
  });
  if (exitcode) {
    std::string msg = TO_STRING("Error executing command: \"" << to_command_line(cmd) << "\"");
    std::cerr << msg << std::endl;
    return absl::InternalError(TO_STRING("Failed to create control points: " << msg));
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
