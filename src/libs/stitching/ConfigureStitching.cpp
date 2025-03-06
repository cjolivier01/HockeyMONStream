#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/Synchronization.h"
#include "hstream/src/libs/common/Process.h"
#include "hstream/src/libs/common/Status.h"

#include "cupano/pano/cudaMat.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include <opencv2/opencv.hpp>

namespace fs = std::filesystem;

extern "C" char** environ;

namespace hm {
namespace stitching {

namespace {

namespace fs = std::filesystem;

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

// -----------------------------------------------------------------------------
// ValidationResult: Returned by the checker function.
//   - valid: true if every child is newer than its parent for every dependency edge.
//   - levels: a list of tree depths (starting at 0 for the root) where at least one violation occurred.
// -----------------------------------------------------------------------------
struct ValidationResult {
  bool valid;
  std::vector<int> levels;
};

// -----------------------------------------------------------------------------
// checkFileDependencies:
// Recursively checks that for every dependency edge, the child file's modification time
// is strictly newer than its parent's modification time. The function accumulates the
// tree level (depth) at which any violation is found.
//
// Note: If a file appears multiple times (i.e. as a child of two different parents), it will be checked
// for each dependency. If a violation occurs in any dependency edge, that level is recorded.
// -----------------------------------------------------------------------------
ValidationResult checkFileDependencies(const FileNode& node, int level = 0) {
  ValidationResult result{true, {}};

  // Retrieve parent's modification time.
  std::error_code ec;
  auto parentTime = fs::last_write_time(node.filename, ec);
  if (ec) {
    std::cerr << "Error reading file \"" << node.filename << "\": " << ec.message() << "\n";
    result.valid = false;
    result.levels.push_back(level);
    // Even if the parent's time is not available, we continue to check its children.
  }

  // Process each child dependency edge.
  for (const auto& child : node.children) {
    // Get the child's modification time.
    auto childTime = fs::last_write_time(child.filename, ec);
    if (ec) {
      std::cerr << "Error reading file \"" << child.filename << "\": " << ec.message() << "\n";
      result.valid = false;
      result.levels.push_back(level + 1);
    } else if (!fs::exists(node.filename)) {
      // If the parent's file does not exist.
      std::cerr << "Parent file \"" << node.filename << "\" does not exist.\n";
      result.valid = false;
      result.levels.push_back(level);
    } else if (childTime < parentTime) {
      // Violation: child file is not newer than the parent.
      std::cerr << "Violation: \"" << child.filename << "\" (time: " << childTime.time_since_epoch().count()
                << ") is older than its parent \"" << node.filename
                << "\" (time: " << parentTime.time_since_epoch().count() << ").\n";
      result.valid = false;
      result.levels.push_back(level + 1);
    }
    // Recursively check the child's dependency subtree.
    ValidationResult childResult = checkFileDependencies(child, level + 1);
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
bool test_dependency_tree() {
  // Build an example dependency tree.
  FileNode tree{
      "root.txt",
      {{"child1.txt", {{"grandchild1.txt", {}}}},
       {"child2.txt",
        {
            {"grandchild1.txt", {}} // Same file appears as a child of both child1 and child2.
        }}}};

  // Perform the dependency check.
  ValidationResult res = checkFileDependencies(tree);

  if (res.valid) {
    std::cout << "All file dependencies are valid: every child's file is newer than its parent.\n";
  } else {
    // Optionally remove duplicates and sort.
    std::sort(res.levels.begin(), res.levels.end());
    res.levels.erase(std::unique(res.levels.begin(), res.levels.end()), res.levels.end());
    std::cout << "Dependency violations found at level(s): ";
    for (int lvl : res.levels) {
      std::cout << lvl << " ";
    }
    std::cout << "\n";
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

std::string get_game_id(const std::string& game_dir) {
  return fs::path(game_dir).filename();
}

std::unordered_map<std::string, std::string> get_environment() {
  std::unordered_map<std::string, std::string> env_vars;
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

} // namespace

absl::StatusOr<bool> is_stitching_configured(const std::string& game_dir) {
  // return test_dependency_tree();
  return false;
}

bool can_configure_stitching(const YAML::Node& config) {
  return true;
}

absl::StatusOr<Synchronization> calculate_stitching_synchronization(
    const std::string& video1,
    const std::string& video2) {

  auto frame_offsets = synchronize_by_audio(video1, video2);

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
}

absl::Status create_control_points(
    const std::string& game_dir,
    surface::Surface left_surface,
    surface::Surface right_surface) {
  fs::path left_file = fs::path(game_dir) / "left.png";
  fs::path right_file = fs::path(game_dir) / "right.png";
  HM_RETURN_IF_ERROR(save_image(left_surface, left_file));
  HM_RETURN_IF_ERROR(save_image(right_surface, right_file));

  fs::path hm_cupano_dir = fs::path("external") / "hm-cupano";

  std::vector<std::string> cmd{
      "/home/colivier/miniforge3/envs/ubuntu/bin/python",
      fs::path("scripts") / "create_control_points.py",
      "--left",
      left_file,
      "--right",
      right_file,
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

absl::Status configure_stitching(
    const std::string& game_dir,
    surface::Surface left_surface,
    surface::Surface right_surface) {
  HM_RETURN_IF_ERROR(create_control_points(game_dir, left_surface, right_surface));
  return absl::OkStatus();
}

} // namespace stitching
} // namespace hm
