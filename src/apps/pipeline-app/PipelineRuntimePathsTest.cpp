#include "PipelineRuntimePaths.h"

#include <filesystem>
#include <fstream>
#include <iostream>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}

} // namespace

int main() {
  std::string pattern = (fs::temp_directory_path() / "hstream-runtime-paths-test-XXXXXX").string();
  if (::mkdtemp(pattern.data()) == nullptr) {
    std::cerr << "Could not create the runtime-path test directory\n";
    return 1;
  }
  const fs::path temporary_root(pattern);
  const fs::path workspace = temporary_root / "workspace";
  const fs::path execroot = temporary_root / "output-base/execroot/synthetic";
  const fs::path immutable_bazel_bin = execroot / "bazel-out/k8-opt/bin";
  const fs::path executable = immutable_bazel_bin / "src/apps/pipeline-app/hstream-cli";
  const fs::path mutable_bazel_bin = workspace / "bazel-bin";
  fs::create_directories(executable.parent_path());
  fs::create_directories(mutable_bazel_bin / "src/gst-plugins");
  fs::create_directories(workspace / "configs");
  fs::create_directories(workspace / "src");
  fs::create_directory_symlink(workspace / "src", execroot / "src");
  std::ofstream(executable) << "synthetic executable\n";
  std::ofstream(workspace / "WORKSPACE.bazel") << "workspace(name = \"synthetic\")\n";

  const hm::pipeline_internal::RuntimePaths selected =
      hm::pipeline_internal::select_runtime_paths(executable, workspace);
  bool ok = expect(fs::equivalent(selected.root, workspace), "A Bazel CLI must recover its source workspace") &&
      expect(fs::equivalent(selected.bazel_bin, immutable_bazel_bin),
             "A Bazel CLI must retain its immutable output tree instead of the mutable workspace bazel-bin") &&
      expect(selected.bazel_output, "A CLI under bazel-out must be identified as a Bazel runtime") &&
      expect(selected.output_configuration == "k8-opt", "The Bazel output configuration must be retained") &&
      expect(!fs::equivalent(selected.bazel_bin, mutable_bazel_bin),
             "Retargeting workspace bazel-bin must not change the running CLI's plugin/runtime-library source");
  fs::remove(execroot / "src");
  const hm::pipeline_internal::RuntimePaths selected_without_symlink_forest =
      hm::pipeline_internal::select_runtime_paths(executable, workspace);
  ok &= expect(
            fs::equivalent(selected_without_symlink_forest.root, workspace),
            "A Bazel CLI must recover its working source workspace when the execroot symlink forest is absent") &&
      expect(
            fs::equivalent(selected_without_symlink_forest.bazel_bin, immutable_bazel_bin),
            "An absent execroot symlink forest must not redirect the CLI to the mutable workspace bazel-bin") &&
      expect(
            selected_without_symlink_forest.bazel_output,
            "A CLI under bazel-out must remain a Bazel runtime when the execroot symlink forest is absent") &&
      expect(
            selected_without_symlink_forest.output_configuration == "k8-opt",
            "The immutable output configuration must survive an absent execroot symlink forest");
  fs::remove_all(temporary_root);
  return ok ? 0 : 1;
}
