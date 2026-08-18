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
  const fs::path immutable_bazel_bin = workspace / "bazel-out/k8-opt/bin";
  const fs::path executable = immutable_bazel_bin / "src/apps/pipeline-app/hstream-cli";
  const fs::path mutable_bazel_bin = workspace / "bazel-bin";
  fs::create_directories(executable.parent_path());
  fs::create_directories(mutable_bazel_bin / "src/gst-plugins");
  fs::create_directories(workspace / "configs");
  std::ofstream(executable) << "synthetic executable\n";
  std::ofstream(workspace / "WORKSPACE.bazel") << "workspace(name = \"synthetic\")\n";

  const hm::pipeline_internal::RuntimePaths selected =
      hm::pipeline_internal::select_runtime_paths(executable, workspace);
  const bool ok = expect(fs::equivalent(selected.root, workspace), "A Bazel CLI must recover its source workspace") &&
      expect(fs::equivalent(selected.bazel_bin, immutable_bazel_bin),
             "A Bazel CLI must retain its immutable output tree instead of the mutable workspace bazel-bin") &&
      expect(!fs::equivalent(selected.bazel_bin, mutable_bazel_bin),
             "Retargeting workspace bazel-bin must not change the running CLI's plugin/runtime-library source");
  fs::remove_all(temporary_root);
  return ok ? 0 : 1;
}
