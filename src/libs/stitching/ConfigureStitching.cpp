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
#include "absl/strings/str_split.h"

namespace fs = std::filesystem;

extern "C" char** environ;

namespace hm {
namespace stitching {

namespace {

const std::string lfo_prefix = "Left frame offset: ";
const std::string rfo_prefix = "Right frame offset: ";

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
    "mapping_0000.tif,mapping_0000_x.tif,mapping_0000_y.tif,mapping_0001.tif,mapping_0001_x.tif,mapping_0001_y.tif,panorama.tif,seam_file.png";
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

  // Handle cases where the path is empty or refers to the root directory
  if (path.empty() || path.parent_path() == path) {
    return path.filename().string();
  }

  return path.parent_path().filename().string();
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

std::string get_python_interp() {
  auto python_exec = findExecutable("python3", {"PATH"});
  if (!python_exec) {
    return "/usr/bin/python";
  }
  return *python_exec;
}

std::map<std::string, std::string> python_env(const std::string& add_dir, std::map<std::string, std::string> prev) {
  std::string pythonpath = prev["PYTHONPATH"];
  const char* p = getenv("PYTHONPATH");
  if (p && *p) {
    if (!pythonpath.empty()) {
      pythonpath += ':';
    }
    pythonpath += p;
  }
  if (pythonpath.empty()) {
    pythonpath = add_dir;
  } else {
    pythonpath = add_dir + ':' + pythonpath;
  }
  prev["PYTHONPATH"] = pythonpath;
  return prev;
}

} // namespace

absl::StatusOr<bool> is_stitching_configured(const std::string& game_dir) {
  bool up_to_date = test_dependency_tree(game_dir, /*add_rink_mask=*/false);
  if (up_to_date) {
    return true;
  }
  return false;
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

absl::Status create_control_points(
    const std::string& game_dir,
    surface::Surface left_surface,
    surface::Surface right_surface) {
  fs::path left_file = fs::path(game_dir) / "left.png";
  fs::path right_file = fs::path(game_dir) / "right.png";
  HM_RETURN_IF_ERROR(save_image(left_surface, left_file));
  HM_RETURN_IF_ERROR(save_image(right_surface, right_file));

  const fs::path hm_cupano_dir = fs::path("external") / "hm-cupano";

  std::vector<std::string> cmd{
      get_python_interp(),
      fs::path("scripts") / "create_control_points.py",
      "--left",
      left_file,
      "--right",
      right_file,
      "--max-control-points=500",
      //"--max-control-points=1500",
#ifdef __aarch64__
      "--scale=0.6",
#endif
  };
  int exitcode = run_command(
      cmd, hm_cupano_dir, get_environment(), [](const std::string& stderr, const std::string& stdout) -> void {
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

bool is_field_mask_configured(const std::string& game_dir) {
  return test_dependency_tree(game_dir, /*add_rink_mask=*/true);
}

absl::Status create_field_mask(const std::string& game_dir, surface::Surface surface) {
  const fs::path hockeymom_dir = fs::path("external") / "hm";
  fs::path stitched_file = fs::path(game_dir) / "s.png";
  HM_RETURN_IF_ERROR(save_image(surface, stitched_file));
  std::string game_id = get_game_id(stitched_file);
  std::vector<std::string> cmd{
      fs::path("hmfind_ice_rink"),
      "--game-id",
      game_id,
#ifdef __aarch64__
      "--device=cuda",
#else
      "--device=cpu",
#endif
  };

  int exitcode = run_command(
      cmd,
      hockeymom_dir,
      python_env(".", get_environment()),
      [](const std::string& stderr, const std::string& stdout) -> void {
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
  const fs::path hockeymom_dir = fs::path("external") / "hm";
  std::string game_id = get_game_id(game_dir);
  std::vector<std::string> cmd{
      get_python_interp(),
      fs::path("hmlib/orientation.py"),
      "--game-id",
      game_id,
  };
  auto env = python_env(".", get_environment());
  int exitcode = run_command(cmd, hockeymom_dir, env, [](const std::string& stderr, const std::string& stdout) -> void {
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
