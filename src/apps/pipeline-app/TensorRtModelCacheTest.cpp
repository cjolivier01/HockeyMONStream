#include "TensorRtModelCache.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>
#include <yaml-cpp/yaml.h>

namespace {

namespace fs = std::filesystem;

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

} // namespace

int main() {
  bool ok = true;
  const fs::path root = fs::temp_directory_path() / ("hmstream-trt-cache-test-" + std::to_string(::getpid()));
  const fs::path configs = root / "configs";
  const fs::path models = root / "packaged-models";
  const fs::path cache = root / "cache";
  fs::create_directories(configs);
  fs::create_directories(models);
  {
    std::ofstream(models / "detector.onnx") << "test onnx model\n";
    std::ofstream(models / "labels.txt") << "person\n";
    std::ofstream(models / "custom.so") << "test library\n";
    std::ofstream inference(configs / "infer.yaml");
    inference << "property:\n"
                 "  onnx-file: ../packaged-models/detector.onnx\n"
                 "  model-engine-file: /tmp/detector.onnx_b2_gpu0_fp32.engine\n"
                 "  labelfile-path: ../packaged-models/labels.txt\n"
                 "  custom-lib-path: ../packaged-models/custom.so\n";
  }
  fs::permissions(
      models,
      fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read | fs::perms::group_exec |
          fs::perms::others_read | fs::perms::others_exec);
  ::setenv("HMSTREAM_TENSORRT_CACHE_DIR", cache.c_str(), 1);

  YAML::Node pipeline;
  pipeline["primary-gie"]["enable"] = 1;
  pipeline["primary-gie"]["config-file"] = "infer.yaml";
  const auto status = hm::pipeline::PrepareTensorRtModelCache(pipeline, configs);
  ok &= expect(status.ok(), "read-only packaged ONNX must be redirected to a writable cache");
  const fs::path runtime_config = pipeline["primary-gie"]["config-file"].as<std::string>();
  ok &= expect(runtime_config != configs / "infer.yaml", "pipeline must use the cached runtime inference config");
  ok &= expect(fs::is_regular_file(runtime_config), "runtime inference config must be published");
  if (fs::is_regular_file(runtime_config)) {
    const YAML::Node cached = YAML::LoadFile(runtime_config.string());
    const fs::path cached_onnx = cached["property"]["onnx-file"].as<std::string>();
    const fs::path cached_engine = cached["property"]["model-engine-file"].as<std::string>();
    std::error_code error;
    ok &= expect(
        fs::equivalent(cached_onnx, models / "detector.onnx", error) && !error,
        "cached ONNX should use a zero-copy hard link when the filesystems match");
    ok &= expect(
        !fs::is_symlink(fs::symlink_status(cached_onnx)),
        "cached ONNX must not resolve back into a read-only package through a symlink");
    ok &= expect(
        cached_engine.parent_path() == cached_onnx.parent_path(),
        "DeepStream engine and cached ONNX must share a writable directory");
    ok &= expect(
        cached["property"]["labelfile-path"].as<std::string>() == (models / "labels.txt").string(),
        "relative label path must remain valid after moving the runtime config");
    ok &= expect(
        cached["property"]["custom-lib-path"].as<std::string>() == (models / "custom.so").string(),
        "relative custom library path must remain valid after moving the runtime config");
  }

  YAML::Node disabled;
  disabled["primary-gie"]["enable"] = 0;
  disabled["primary-gie"]["config-file"] = "infer.yaml";
  ok &= expect(
      hm::pipeline::PrepareTensorRtModelCache(disabled, configs).ok(), "disabled inference section must be ignored");
  ok &= expect(
      disabled["primary-gie"]["config-file"].as<std::string>() == "infer.yaml",
      "disabled inference config must not be rewritten");

  fs::permissions(models, fs::perms::owner_all);
  YAML::Node writable;
  writable["primary-gie"]["enable"] = 1;
  writable["primary-gie"]["config-file"] = "infer.yaml";
  ok &= expect(
      hm::pipeline::PrepareTensorRtModelCache(writable, configs).ok(),
      "writable development model directory must remain supported");
  ok &= expect(
      writable["primary-gie"]["config-file"].as<std::string>() == "infer.yaml",
      "development inference config must not be redirected unnecessarily");

  ::unsetenv("HMSTREAM_TENSORRT_CACHE_DIR");
  fs::remove_all(root);
  return ok ? 0 : 1;
}
