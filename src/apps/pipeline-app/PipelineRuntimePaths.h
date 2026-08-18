#pragma once

#include <filesystem>

namespace hm::pipeline_internal {

struct RuntimePaths {
  std::filesystem::path root;
  std::filesystem::path bazel_bin;
};

RuntimePaths select_runtime_paths(
    const std::filesystem::path& executable,
    const std::filesystem::path& working_directory);

} // namespace hm::pipeline_internal
