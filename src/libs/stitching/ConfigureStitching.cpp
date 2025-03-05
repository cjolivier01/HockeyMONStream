#include "hstream/libs/stitching/ConfigureStitching.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace hm {
namespace stitching {

namespace {

namespace fs = std::filesystem;

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

} // namespace

absl::StatusOr<bool> is_stitching_configured(const std::string& game_dir) {
  // return test_dependency_tree();
  return false;
}

bool can_configure_stitching(const YAML::Node& config) {
  return true;
}

absl::Status configure_stitching(
    const std::string& game_id,
    surface::Surface left_surface,
    surface::Surface right_surface) {
  return absl::OkStatus();
}

} // namespace stitching
} // namespace hm
