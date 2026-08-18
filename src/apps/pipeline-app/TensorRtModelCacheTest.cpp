#include "TensorRtModelCache.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

namespace {

namespace fs = std::filesystem;

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

void write_inference_config(const fs::path& path, int network_mode) {
  std::ofstream inference(path);
  inference << "property:\n"
               "  onnx-file: ../packaged-models/detector.onnx\n"
               "  model-engine-file: /tmp/not-the-derived-name.engine\n"
               "  labelfile-path: ../packaged-models/labels.txt\n"
               "  custom-lib-path: ../packaged-models/custom.so\n"
               "  batch-size: 2\n"
               "  gpu-id: 0\n"
               "  network-mode: "
            << network_mode << '\n';
}

YAML::Node pipeline_for(const std::string& config_file) {
  YAML::Node pipeline;
  pipeline["primary-gie"]["enable"] = 1;
  pipeline["primary-gie"]["config-file"] = config_file;
  return pipeline;
}

} // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--lock-probe") {
    const fs::path root = argv[2];
    std::ofstream(root / "lock-probe-ready") << "ready\n";
    YAML::Node pipeline = pipeline_for("infer.yaml");
    const auto status = hm::pipeline::PrepareTensorRtModelCache(pipeline, root / "configs");
    hm::pipeline::ReleaseTensorRtModelCacheLocks();
    return status.ok() ? 0 : 1;
  }

  bool ok = true;
  const fs::path root =
      fs::weakly_canonical(fs::temp_directory_path()) / ("hstream-trt-cache-test-" + std::to_string(::getpid()));
  const fs::path configs = root / "configs";
  const fs::path models = root / "packaged-models";
  const fs::path cache = root / "cache";
  fs::create_directories(configs);
  fs::create_directories(models);
  {
    std::ofstream(models / "detector.onnx") << "test onnx model\n";
    std::ofstream(models / "labels.txt") << "person\n";
    std::ofstream(models / "custom.so") << "test library\n";
    std::ofstream(models / "detector_bf16.engine") << "prebuilt BF16 engine\n";
    fs::create_symlink("detector.onnx", models / "linked-detector.onnx");
    write_inference_config(configs / "infer.yaml", 0);
    std::ofstream(configs / "loader.yaml") << "property:\n"
                                              "  onnx-file: ../packaged-models/detector.onnx\n"
                                              "  model-engine-file: ../packaged-models/loader.engine\n"
                                              "  custom-lib-path: libnvdsinfer_custom_impl_Yolo.so\n"
                                              "  batch-size: 2\n"
                                              "  gpu-id: 0\n"
                                              "  network-mode: 0\n";
    std::ofstream(configs / "writable-detector.onnx") << "writable test model\n";
    std::ofstream(configs / "loader-writable.yaml") << "property:\n"
                                                       "  onnx-file: writable-detector.onnx\n"
                                                       "  model-engine-file: writable-detector.engine\n"
                                                       "  custom-lib-path: libnvdsinfer_custom_impl_Yolo.so\n"
                                                       "  batch-size: 2\n"
                                                       "  gpu-id: 0\n"
                                                       "  network-mode: 0\n";
    std::ofstream(configs / "infer.txt") << "[property]\nonnx-file=detector.onnx\n";
    std::ofstream(configs / "linked.yaml") << "property:\n"
                                              "  onnx-file: ../packaged-models/linked-detector.onnx\n"
                                              "  model-engine-file: /tmp/linked.engine\n";
    std::ofstream(configs / "bf16.yaml") << "property:\n"
                                            "  onnx-file: ../packaged-models/detector.onnx\n"
                                            "  model-engine-file: ../packaged-models/detector_bf16.engine\n"
                                            "  network-mode: 0\n";
    std::ofstream(configs / "missing-bf16.yaml") << "property:\n"
                                                    "  onnx-file: ../packaged-models/detector.onnx\n"
                                                    "  model-engine-file: ../packaged-models/missing_bf16.engine\n"
                                                    "  network-mode: 0\n";
  }
  fs::permissions(
      models,
      fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read | fs::perms::group_exec |
          fs::perms::others_read | fs::perms::others_exec);
  ::setenv("HSTREAM_TENSORRT_CACHE_DIR", cache.c_str(), 1);

  YAML::Node ini_pipeline = pipeline_for("infer.txt");
  ok &= expect(
      hm::pipeline::PrepareTensorRtModelCache(ini_pipeline, configs).ok(),
      "standard DeepStream INI inference config must be preserved");
  ok &= expect(
      ini_pipeline["primary-gie"]["config-file"].as<std::string>() == "infer.txt",
      "non-YAML inference config must not be rewritten");

  YAML::Node prebuilt_pipeline = pipeline_for("bf16.yaml");
  ok &= expect(
      hm::pipeline::PrepareTensorRtModelCache(prebuilt_pipeline, configs).ok(),
      "an existing prebuilt BF16 engine must remain usable from a read-only package");
  ok &= expect(
      prebuilt_pipeline["primary-gie"]["config-file"].as<std::string>() == "bf16.yaml",
      "prebuilt engine config must not be silently redirected to an FP32 rebuild");
  YAML::Node missing_bf16_pipeline = pipeline_for("missing-bf16.yaml");
  ok &= expect(
      !hm::pipeline::PrepareTensorRtModelCache(missing_bf16_pipeline, configs).ok(),
      "a missing BF16-only prebuilt engine must fail instead of silently rebuilding FP32");

  YAML::Node linked_pipeline = pipeline_for("linked.yaml");
  ok &= expect(
      !hm::pipeline::PrepareTensorRtModelCache(linked_pipeline, configs).ok(),
      "packaged ONNX symlinks must be rejected instead of reproduced in the cache");

  YAML::Node pipeline = pipeline_for("infer.yaml");
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
        cached_engine.filename() == "detector.onnx_b2_gpu0_fp32.engine",
        "engine path must use DeepStream's ONNX-derived filename rather than the configured seed name");
    ok &= expect(
        cached["property"]["labelfile-path"].as<std::string>() == (models / "labels.txt").string(),
        "relative label path must remain valid after moving the runtime config");
    ok &= expect(
        cached["property"]["custom-lib-path"].as<std::string>() == (models / "custom.so").string(),
        "relative custom library path must remain valid after moving the runtime config");
  }

  const pid_t lock_probe = ::fork();
  if (lock_probe == 0) {
    ::execl("/proc/self/exe", "/proc/self/exe", "--lock-probe", root.c_str(), nullptr);
    _exit(127);
  }
  bool probe_ready = false;
  for (int attempt = 0; attempt < 100 && !probe_ready; ++attempt) {
    probe_ready = fs::exists(root / "lock-probe-ready");
    if (!probe_ready)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ok &= expect(probe_ready, "concurrent cache probe must start");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  int probe_status = 0;
  ok &= expect(
      ::waitpid(lock_probe, &probe_status, WNOHANG) == 0,
      "concurrent cold-cache process must wait for the engine initialization lock");
  hm::pipeline::ReleaseTensorRtModelCacheLocks();
  ok &= expect(
      ::waitpid(lock_probe, &probe_status, 0) == lock_probe && WIFEXITED(probe_status) &&
          WEXITSTATUS(probe_status) == 0,
      "concurrent cache probe must continue after the engine lock is released");

  YAML::Node loader_pipeline = pipeline_for("loader.yaml");
  const fs::path parser_binary = root / "libnvdsinfer_custom_impl_Yolo.so";
  std::ofstream(parser_binary) << "staged test parser\n";
  const fs::path runtime_libraries = root / "runtime-libraries";
  fs::create_directories(runtime_libraries);
  const fs::path staged_yolo = runtime_libraries / "libnvdsinfer_custom_impl_Yolo.so";
  fs::create_symlink(parser_binary, staged_yolo);
  ::setenv("HSTREAM_NVINFER_CUSTOM_LIBRARY_DIR", runtime_libraries.c_str(), 1);
  ok &= expect(
      hm::pipeline::PrepareTensorRtModelCache(loader_pipeline, configs).ok(),
      "a loader-resolved custom inference library must prepare successfully");
  const fs::path loader_runtime = loader_pipeline["primary-gie"]["config-file"].as<std::string>();
  fs::path loader_engine;
  if (fs::is_regular_file(loader_runtime)) {
    const YAML::Node loader_cached = YAML::LoadFile(loader_runtime.string());
    loader_engine = loader_cached["property"]["model-engine-file"].as<std::string>();
    ok &= expect(
        loader_cached["property"]["custom-lib-path"].as<std::string>() == staged_yolo.string(),
        "a bare custom library name must resolve to the staged runtime library in the runtime inference config");
  }
  YAML::Node writable_loader_pipeline = pipeline_for("loader-writable.yaml");
  ok &= expect(
      hm::pipeline::PrepareTensorRtModelCache(writable_loader_pipeline, configs).ok(),
      "a staged custom library must prepare even when the ONNX model directory is writable");
  const fs::path writable_loader_runtime = writable_loader_pipeline["primary-gie"]["config-file"].as<std::string>();
  ok &= expect(
      writable_loader_runtime != configs / "loader-writable.yaml" && fs::is_regular_file(writable_loader_runtime),
      "a writable model must still receive a runtime inference config for its staged custom library");
  if (fs::is_regular_file(writable_loader_runtime)) {
    const YAML::Node writable_loader_cached = YAML::LoadFile(writable_loader_runtime.string());
    ok &= expect(
        writable_loader_cached["property"]["custom-lib-path"].as<std::string>() == staged_yolo.string(),
        "the writable-model runtime config must use the staged custom library path");
    ok &= expect(
        writable_loader_cached["property"]["onnx-file"].as<std::string>() ==
            fs::absolute(configs / "writable-detector.onnx").lexically_normal().string(),
        "moving a staged-parser config must preserve its relative ONNX path");
    ok &= expect(
        writable_loader_cached["property"]["model-engine-file"].as<std::string>() ==
            fs::absolute(configs / "writable-detector.engine").lexically_normal().string(),
        "moving a staged-parser config must preserve its relative engine path");
  }

  const fs::path second_runtime_libraries = root / "runtime-libraries-second-launch";
  fs::create_directories(second_runtime_libraries);
  const fs::path second_staged_yolo = second_runtime_libraries / "libnvdsinfer_custom_impl_Yolo.so";
  fs::create_symlink(parser_binary, second_staged_yolo);
  ::setenv("HSTREAM_NVINFER_CUSTOM_LIBRARY_DIR", second_runtime_libraries.c_str(), 1);
  YAML::Node second_loader_pipeline = pipeline_for("loader.yaml");
  ok &= expect(
      hm::pipeline::PrepareTensorRtModelCache(second_loader_pipeline, configs).ok(),
      "the same parser staged for a second launch must prepare successfully");
  const fs::path second_loader_runtime = second_loader_pipeline["primary-gie"]["config-file"].as<std::string>();
  if (fs::is_regular_file(second_loader_runtime)) {
    const YAML::Node second_loader_cached = YAML::LoadFile(second_loader_runtime.string());
    ok &= expect(
        second_loader_cached["property"]["custom-lib-path"].as<std::string>() == second_staged_yolo.string(),
        "the second launch runtime config must use its own staged custom library path");
    ok &= expect(
        !loader_engine.empty() &&
            fs::path(second_loader_cached["property"]["model-engine-file"].as<std::string>()) == loader_engine,
        "identical parser contents in different launch directories must reuse the TensorRT engine cache identity");
  }

  std::ofstream(parser_binary, std::ios::trunc) << "changed staged test parser\n";
  const fs::path third_runtime_libraries = root / "runtime-libraries-changed-parser";
  fs::create_directories(third_runtime_libraries);
  const fs::path third_staged_yolo = third_runtime_libraries / "libnvdsinfer_custom_impl_Yolo.so";
  fs::create_symlink(parser_binary, third_staged_yolo);
  ::setenv("HSTREAM_NVINFER_CUSTOM_LIBRARY_DIR", third_runtime_libraries.c_str(), 1);
  YAML::Node changed_parser_pipeline = pipeline_for("loader.yaml");
  ok &= expect(
      hm::pipeline::PrepareTensorRtModelCache(changed_parser_pipeline, configs).ok(),
      "a changed parser staged for another launch must prepare successfully");
  const fs::path changed_parser_runtime = changed_parser_pipeline["primary-gie"]["config-file"].as<std::string>();
  if (fs::is_regular_file(changed_parser_runtime)) {
    const YAML::Node changed_parser_cached = YAML::LoadFile(changed_parser_runtime.string());
    ok &= expect(
        fs::path(changed_parser_cached["property"]["model-engine-file"].as<std::string>()) != loader_engine,
        "changed parser contents must select a fresh TensorRT engine cache identity");
  }
  ::unsetenv("HSTREAM_NVINFER_CUSTOM_LIBRARY_DIR");
  hm::pipeline::ReleaseTensorRtModelCacheLocks();

  const fs::path fp32_runtime_directory = runtime_config.parent_path();
  write_inference_config(configs / "infer.yaml", 2);
  YAML::Node changed_pipeline = pipeline_for("infer.yaml");
  ok &= expect(
      hm::pipeline::PrepareTensorRtModelCache(changed_pipeline, configs).ok(),
      "changed inference build config must prepare successfully");
  const fs::path changed_runtime = changed_pipeline["primary-gie"]["config-file"].as<std::string>();
  ok &= expect(
      changed_runtime.parent_path() != fp32_runtime_directory,
      "same-path inference build changes must select a fresh engine cache identity");
  if (fs::is_regular_file(changed_runtime)) {
    const YAML::Node changed = YAML::LoadFile(changed_runtime.string());
    ok &= expect(
        fs::path(changed["property"]["model-engine-file"].as<std::string>()).filename() ==
            "detector.onnx_b2_gpu0_fp16.engine",
        "effective network mode must be reflected in DeepStream's derived engine filename");
  }
  hm::pipeline::ReleaseTensorRtModelCacheLocks();

  write_inference_config(configs / "infer.yaml", 0);
  YAML::Node overridden_pipeline = pipeline_for("infer.yaml");
  overridden_pipeline["application"]["global-gpu-id"] = 1;
  overridden_pipeline["application"]["use-nvmultiurisrcbin"] = 1;
  overridden_pipeline["application"]["max-batch-size"] = 4;
  overridden_pipeline["primary-gie"]["batch-size"] = 2;
  overridden_pipeline["primary-gie"]["model-engine-file"] = "/tmp/section-seed.engine";
  ok &= expect(
      hm::pipeline::PrepareTensorRtModelCache(overridden_pipeline, configs).ok(),
      "application and section-level GIE overrides must prepare successfully");
  const fs::path overridden_runtime = overridden_pipeline["primary-gie"]["config-file"].as<std::string>();
  if (fs::is_regular_file(overridden_runtime)) {
    const YAML::Node overridden = YAML::LoadFile(overridden_runtime.string());
    const fs::path effective_engine = overridden["property"]["model-engine-file"].as<std::string>();
    ok &= expect(
        effective_engine.filename() == "detector.onnx_b4_gpu1_fp32.engine",
        "global GPU and nvmultiurisrc batch overrides must determine DeepStream's engine filename");
    ok &= expect(
        overridden_pipeline["primary-gie"]["model-engine-file"].as<std::string>() == effective_engine.string(),
        "section-level model-engine-file must be rewritten to the locked cache engine");
    ok &= expect(
        overridden_runtime.parent_path() != fp32_runtime_directory,
        "application-level engine build overrides must participate in cache identity");
  }
  hm::pipeline::ReleaseTensorRtModelCacheLocks();

  YAML::Node secondary_pipeline;
  secondary_pipeline["application"]["global-gpu-id"] = 2;
  secondary_pipeline["application"]["use-nvmultiurisrcbin"] = 1;
  secondary_pipeline["application"]["sgie-batch-size"] = 8;
  secondary_pipeline["secondary-gie0"]["enable"] = 1;
  secondary_pipeline["secondary-gie0"]["batch-size"] = 16;
  secondary_pipeline["secondary-gie0"]["config-file"] = "infer.yaml";
  ok &= expect(
      hm::pipeline::PrepareTensorRtModelCache(secondary_pipeline, configs).ok(),
      "secondary GIE application overrides must prepare successfully");
  const fs::path secondary_runtime = secondary_pipeline["secondary-gie0"]["config-file"].as<std::string>();
  if (fs::is_regular_file(secondary_runtime)) {
    const YAML::Node secondary = YAML::LoadFile(secondary_runtime.string());
    ok &= expect(
        fs::path(secondary["property"]["model-engine-file"].as<std::string>()).filename() ==
            "detector.onnx_b8_gpu2_fp32.engine",
        "nvmultiurisrc SGIE batch and global GPU overrides must determine the secondary engine filename");
    ok &= expect(
        secondary_runtime.parent_path() != overridden_runtime.parent_path(),
        "primary and secondary effective engine contexts must not overwrite one runtime config");
  }
  hm::pipeline::ReleaseTensorRtModelCacheLocks();

  YAML::Node primary_omitted_batch = pipeline_for("infer.yaml");
  primary_omitted_batch["application"]["use-nvmultiurisrcbin"] = 1;
  primary_omitted_batch["application"]["max-batch-size"] = 4;
  ok &= expect(
      hm::pipeline::PrepareTensorRtModelCache(primary_omitted_batch, configs).ok(),
      "primary GIE without a section batch override must prepare successfully");
  const fs::path primary_omitted_runtime = primary_omitted_batch["primary-gie"]["config-file"].as<std::string>();
  if (fs::is_regular_file(primary_omitted_runtime)) {
    const YAML::Node primary_omitted = YAML::LoadFile(primary_omitted_runtime.string());
    ok &= expect(
        fs::path(primary_omitted["property"]["model-engine-file"].as<std::string>()).filename() ==
            "detector.onnx_b2_gpu0_fp32.engine",
        "nvmultiurisrc must retain the inference batch when the primary section does not enable its override");
  }
  hm::pipeline::ReleaseTensorRtModelCacheLocks();

  YAML::Node secondary_omitted_batch;
  secondary_omitted_batch["application"]["use-nvmultiurisrcbin"] = 1;
  secondary_omitted_batch["application"]["sgie-batch-size"] = 8;
  secondary_omitted_batch["secondary-gie0"]["enable"] = 1;
  secondary_omitted_batch["secondary-gie0"]["config-file"] = "infer.yaml";
  ok &= expect(
      hm::pipeline::PrepareTensorRtModelCache(secondary_omitted_batch, configs).ok(),
      "secondary GIE without a section batch override must prepare successfully");
  const fs::path secondary_omitted_runtime = secondary_omitted_batch["secondary-gie0"]["config-file"].as<std::string>();
  if (fs::is_regular_file(secondary_omitted_runtime)) {
    const YAML::Node secondary_omitted = YAML::LoadFile(secondary_omitted_runtime.string());
    ok &= expect(
        fs::path(secondary_omitted["property"]["model-engine-file"].as<std::string>()).filename() ==
            "detector.onnx_b2_gpu0_fp32.engine",
        "nvmultiurisrc must retain the inference batch when a secondary section does not enable its override");
  }
  hm::pipeline::ReleaseTensorRtModelCacheLocks();

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

  ::unsetenv("HSTREAM_TENSORRT_CACHE_DIR");
  hm::pipeline::ReleaseTensorRtModelCacheLocks();
  fs::remove_all(root);
  return ok ? 0 : 1;
}
