#include "PipelineRuntimePaths.h"

#include <optional>

namespace fs = std::filesystem;

namespace {

fs::path resolve_path(const fs::path& path) {
  std::error_code error;
  const fs::path canonical = fs::canonical(path, error);
  if (!error)
    return canonical;
  const fs::path absolute = fs::absolute(path, error);
  return error ? path : absolute;
}

std::optional<fs::path> bazel_bin_for_executable(const fs::path& executable) {
  for (fs::path cursor = executable.parent_path(); !cursor.empty(); cursor = cursor.parent_path()) {
    if (cursor.filename() == "bin" && cursor.parent_path().parent_path().filename() == "bazel-out")
      return cursor;
    if (cursor.filename() == "bazel-bin")
      return cursor;
    if (cursor == cursor.root_path())
      break;
  }
  return std::nullopt;
}

std::optional<fs::path> source_root_for_bazel_bin(const fs::path& bazel_bin) {
  if (bazel_bin.filename() == "bazel-bin")
    return bazel_bin.parent_path();
  const fs::path execroot = bazel_bin.parent_path().parent_path().parent_path();
  std::error_code error;
  const fs::path marker = fs::canonical(execroot / "WORKSPACE.bazel", error);
  if (error || marker.empty() || !fs::is_directory(marker.parent_path() / "configs"))
    return std::nullopt;
  return marker.parent_path();
}

std::optional<fs::path> installed_root_for_executable(const fs::path& executable) {
  for (fs::path cursor = executable.parent_path(); !cursor.empty(); cursor = cursor.parent_path()) {
    if (cursor.filename() == "bin" && fs::is_directory(cursor.parent_path() / "configs"))
      return cursor.parent_path();
    if (cursor == cursor.root_path())
      break;
  }
  return std::nullopt;
}

} // namespace

hm::pipeline_internal::RuntimePaths hm::pipeline_internal::select_runtime_paths(
    const fs::path& executable,
    const fs::path& working_directory) {
  const fs::path resolved_executable = resolve_path(executable);
  if (const auto bazel_bin = bazel_bin_for_executable(resolved_executable)) {
    const fs::path resolved_bazel_bin = resolve_path(*bazel_bin);
    if (const auto source_root = source_root_for_bazel_bin(resolved_bazel_bin))
      return {*source_root, resolved_bazel_bin};
  }
  if (const auto installed_root = installed_root_for_executable(resolved_executable))
    return {*installed_root, *installed_root / "bazel-bin"};

  const fs::path root = working_directory.empty() ? fs::current_path() : resolve_path(working_directory);
  return {root, root / "bazel-bin"};
}
