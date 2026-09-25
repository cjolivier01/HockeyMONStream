#include "TensorRtModelCache.h"

#include "OnnxExternalData.h"
#include "hstream/src/libs/assets/AssetManager.h"

#include <chrono>
#include <cstdint>
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

std::string gpu_model = "NVIDIA_Test_GPU";
absl::Status prepare_cache(YAML::Node pipeline, const fs::path& directory) {
  return hm::pipeline::PrepareTensorRtModelCache(pipeline, directory, [](unsigned gpu) -> absl::StatusOr<std::string> {
    return gpu_model + (gpu ? "_" + std::to_string(gpu) : "");
  });
}

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

std::string varint(uint64_t value) {
  std::string result;
  do {
    const unsigned char byte = value & 0x7f;
    value >>= 7;
    result += static_cast<char>(byte | (value ? 0x80 : 0));
  } while (value);
  return result;
}

std::string protobuf_bytes(unsigned field, const std::string& bytes) {
  return varint((field << 3) | 2) + varint(bytes.size()) + bytes;
}

std::string external_tensor(const std::string& location) {
  // data_location may precede external_data; unrelated map entries may follow.
  return varint(14 << 3) + varint(1) + protobuf_bytes(13, protobuf_bytes(1, "location") + protobuf_bytes(2, location)) +
      protobuf_bytes(13, protobuf_bytes(1, "offset") + protobuf_bytes(2, "0"));
}

void write_model(const fs::path& path, const std::string& description, const std::string& graph = {}) {
  std::ofstream(path, std::ios::binary) << varint(1 << 3) << varint(8) << protobuf_bytes(6, description)
                                        << protobuf_bytes(7, graph);
}

void write_inference_config(const fs::path& path, int network_mode) {
  std::ofstream inference(path);
  inference << "property:\n"
               "  onnx-file: ../packaged-models/detector.onnx\n"
               "  model-engine-file: /tmp/not-the-derived-name.engine\n"
               "  labelfile-path: ../packaged-models/labels.txt\n"
               "  custom-lib-path: ../packaged-models/custom.so\n"
               "  custom-network-config: ../packaged-models/network.cfg\n"
               "  tlt-encoded-model: ../packaged-models/detector.etlt\n"
               "  op-tensor-files: ../packaged-models/output-0.tensor;../packaged-models/output-1.tensor\n"
               "  raw-output-file-write: true\n"
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
    const auto status = prepare_cache(pipeline, root / "configs");
    hm::pipeline::ReleaseTensorRtModelCacheLocks();
    return status.ok() ? 0 : 1;
  }

  bool ok = true;
  const fs::path root =
      fs::weakly_canonical(fs::temp_directory_path()) / ("hstream-trt-cache-test-" + std::to_string(::getpid()));
  const fs::path configs = root / "configs";
  const fs::path models = root / "packaged-models";
  const fs::path home = root / "home";
  const fs::path home_models = home / ".cache/hstream/models";
  const fs::path cache = root / "cache";
  fs::create_directories(configs);
  fs::create_directories(models);
  fs::create_directories(home_models);
  {
    write_model(models / "detector.onnx", "test onnx model");
    std::ofstream(models / "labels.txt") << "person\n";
    std::ofstream(models / "custom.so") << "test library\n";
    std::ofstream(models / "network.cfg") << "test network config\n";
    std::ofstream(models / "detector.etlt") << "test TLT model\n";
    std::ofstream(models / "output-0.tensor") << "test output tensor zero\n";
    std::ofstream(models / "output-1.tensor") << "test output tensor one\n";
    std::ofstream(models / "detector_bf16.engine") << "prebuilt BF16 engine\n";
    std::ofstream(models / "detector_int8_calib.table") << "prebuilt INT8 calibration table\n";
    write_model(home_models / "home-detector.onnx", "user-cache ONNX model");
    std::ofstream(home_models / "home-detector.engine") << "explicit prebuilt engine\n";
    fs::create_symlink("detector.onnx", models / "linked-detector.onnx");
    write_inference_config(configs / "infer.yaml", 0);
    std::ofstream(configs / "loader.yaml") << "property:\n"
                                              "  onnx-file: ../packaged-models/detector.onnx\n"
                                              "  model-engine-file: ../packaged-models/loader.engine\n"
                                              "  custom-lib-path: libnvdsinfer_custom_impl_Yolo.so\n"
                                              "  batch-size: 2\n"
                                              "  gpu-id: 0\n"
                                              "  network-mode: 0\n";
    write_model(configs / "writable-detector.onnx", "writable test model");
    std::ofstream(configs / "loader-writable.yaml")
        << "property:\n"
           "  onnx-file: writable-detector.onnx\n"
           "  model-engine-file: writable-detector.engine\n"
           "  custom-lib-path: libnvdsinfer_custom_impl_Yolo.so\n"
           "  custom-network-config: ../packaged-models/network.cfg\n"
           "  tlt-encoded-model: ../packaged-models/detector.etlt\n"
           "  op-tensor-files: "
           "../packaged-models/output-0.tensor;../packaged-models/output-1.tensor\n"
           "  raw-output-file-write: true\n"
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
    std::ofstream(configs / "int8.yaml") << "property:\n"
                                            "  onnx-file: ../packaged-models/detector.onnx\n"
                                            "  model-engine-file: ../packaged-models/missing_int8.engine\n"
                                            "  int8-calib-file: ../packaged-models/detector_int8_calib.table\n"
                                            "  network-mode: 1\n"
                                            "  batch-size: 2\n"
                                            "  gpu-id: 0\n";
    std::ofstream(configs / "missing-int8-calib.yaml")
        << "property:\n"
           "  onnx-file: ../packaged-models/detector.onnx\n"
           "  model-engine-file: ../packaged-models/missing_int8.engine\n"
           "  int8-calib-file: ../packaged-models/missing_calib.table\n"
           "  network-mode: 1\n";
    for (const auto& [name, prefix] : {
             std::pair{"home-dollar.yaml", "$HOME"},
             std::pair{"home-braced.yaml", "${HOME}"},
             std::pair{"home-tilde.yaml", "~"},
         }) {
      std::ofstream(configs / name) << "property:\n"
                                    << "  onnx-file: " << prefix << "/.cache/hstream/models/home-detector.onnx\n"
                                    << "  model-engine-file: " << prefix
                                    << "/.cache/hstream/models/home-detector.engine\n"
                                       "  network-mode: 0\n";
    }
  }
  fs::permissions(
      models,
      fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read | fs::perms::group_exec |
          fs::perms::others_read | fs::perms::others_exec);
  ::setenv("HSTREAM_TENSORRT_CACHE_DIR", cache.c_str(), 1);
  ::setenv("HOME", home.c_str(), 1);

  fs::copy_file(models / "labels.txt", home_models / "labels_coco.txt");
  for (const char* precision : {"bf16", "int8"}) {
    auto bundled = YAML::LoadFile(
        std::string("configs/config_infer_yolov8_hockey_") + precision + ".yaml");
    // Exercise startup asset acquisition before cache preparation, as the CLI
    // does. No source model is installed and a download would fail immediately.
    bundled["property"]["model-engine-file"] = (models / "detector_bf16.engine").string();
    for (auto asset : bundled["pretrained-assets"]) {
      asset["url"] = "https://127.0.0.1:1/must-not-download";
      if (asset["property"].as<std::string>() == "labelfile-path")
        asset["sha256"] = *hm::assets::AssetManager::Sha256(models / "labels.txt");
    }
    const auto config_path = configs / (std::string("bundled-") + precision + ".yaml");
    std::ofstream(config_path) << bundled;
    const auto asset_status = hm::assets::AssetManager::EnsureRequired({config_path});
    if (!asset_status.ok())
      std::cerr << asset_status << '\n';
    ok &= expect(
        asset_status.ok(),
        "prepared detector startup must acquire labels without requiring the offline source model");
    auto pipeline = pipeline_for(config_path.string());
    ok &= expect(
        prepare_cache(pipeline, configs).ok(),
        "bundled prepared detector config must load with only an engine and labels installed");
  }

  for (const char* home_config : {"home-dollar.yaml", "home-braced.yaml", "home-tilde.yaml"}) {
    YAML::Node home_pipeline = pipeline_for(home_config);
    ok &= expect(
        prepare_cache(home_pipeline, configs).ok(),
        "HOME-prefixed paths in inference configs must resolve to the user model cache");
    const fs::path home_runtime = home_pipeline["primary-gie"]["config-file"].as<std::string>();
    ok &= expect(
        home_runtime != configs / home_config && fs::is_regular_file(home_runtime),
        "HOME-prefixed paths must be republished for DeepStream instead of remaining literal");
    if (fs::is_regular_file(home_runtime)) {
      const YAML::Node expanded = YAML::LoadFile(home_runtime.string());
      ok &= expect(
          expanded["property"]["onnx-file"].as<std::string>() == (home_models / "home-detector.onnx").string(),
          "the runtime inference config must contain an absolute HOME-expanded ONNX path");
      ok &= expect(
          expanded["property"]["model-engine-file"].as<std::string>() ==
              (home_models / "home-detector.engine").string(),
          "the runtime inference config must contain an absolute HOME-expanded engine path");
    }
  }

  YAML::Node ini_pipeline = pipeline_for("infer.txt");
  ini_pipeline["primary-gie"]["workspace-size"] = 64;
  ok &= expect(prepare_cache(ini_pipeline, configs).ok(), "standard DeepStream INI inference config must be preserved");
  ok &= expect(
      ini_pipeline["primary-gie"]["config-file"].as<std::string>() == "infer.txt" &&
          !ini_pipeline["primary-gie"]["workspace-size"],
      "non-YAML inference config must not be rewritten or retain the YAML-only workspace override");

  YAML::Node workspace_pipeline = pipeline_for("infer.yaml");
  workspace_pipeline["primary-gie"]["workspace-size"] = 64;
  ok &= expect(prepare_cache(workspace_pipeline, configs).ok(), "inference workspace override must prepare");
  const fs::path workspace_runtime = workspace_pipeline["primary-gie"]["config-file"].as<std::string>();
  ok &= expect(
      !workspace_pipeline["primary-gie"]["workspace-size"] &&
          YAML::LoadFile(workspace_runtime.string())["property"]["workspace-size"].as<int>() == 64,
      "the app-level workspace override must move into the generated inference config");

  YAML::Node prebuilt_pipeline = pipeline_for("bf16.yaml");
  ok &= expect(
      prepare_cache(prebuilt_pipeline, configs).ok(),
      "an existing prebuilt BF16 engine must remain usable from a read-only package");
  const auto prebuilt_config = YAML::LoadFile(prebuilt_pipeline["primary-gie"]["config-file"].as<std::string>());
  ok &= expect(
      !prebuilt_config["property"]["onnx-file"] &&
          prebuilt_config["property"]["model-engine-file"].as<std::string>() ==
              (models / "detector_bf16.engine").string(),
      "BF16 runtime must be engine-only so a failed deserialization cannot rebuild FP32");
  // Explicit quantization has no legacy table and an engine can be deployed
  // without the original model. Metadata must not reach DeepStream's parser.
  std::ofstream(configs / "explicit-int8.yaml")
      << "hstream-prebuilt-precision: int8\n"
         "property:\n  model-engine-file: ../packaged-models/detector_bf16.engine\n"
         "  onnx-file: does-not-exist.onnx\n  int8-calib-file: does-not-exist.table\n"
         "  engine-create-func-name: OldBuilder\n  network-mode: 1\n";
  auto explicit_int8 = pipeline_for("explicit-int8.yaml");
  ok &= expect(
      prepare_cache(explicit_int8, configs).ok(),
      "a prepared INT8 engine needs neither source model nor legacy calibration table");
  const auto explicit_config = YAML::LoadFile(explicit_int8["primary-gie"]["config-file"].as<std::string>());
  ok &= expect(
      !explicit_config["property"]["onnx-file"] && !explicit_config["property"]["int8-calib-file"] &&
          !explicit_config["property"]["engine-create-func-name"] && !explicit_config["hstream-prebuilt-precision"],
      "prebuilt runtime config must remove every available rebuild path");
  auto missing_explicit_int8 = pipeline_for("explicit-int8.yaml");
  missing_explicit_int8["primary-gie"]["model-engine-file"] = "missing.engine";
  ok &= expect(
      !prepare_cache(missing_explicit_int8, configs).ok(),
      "a missing explicit INT8 engine must fail instead of building uncalibrated inference");
  std::ofstream(configs / "empty.engine");
  auto empty_engine = pipeline_for("explicit-int8.yaml");
  empty_engine["primary-gie"]["model-engine-file"] = "empty.engine";
  ok &= expect(!prepare_cache(empty_engine, configs).ok(), "empty prebuilt engines must be rejected");
  YAML::Node missing_bf16_pipeline = pipeline_for("missing-bf16.yaml");
  ok &= expect(
      !prepare_cache(missing_bf16_pipeline, configs).ok(),
      "a missing BF16-only prebuilt engine must fail instead of silently rebuilding FP32");

  YAML::Node int8_pipeline = pipeline_for("int8.yaml");
  ok &= expect(
      prepare_cache(int8_pipeline, configs).ok(),
      "an INT8 inference config with a calibration table may prepare a cached engine");
  const fs::path int8_runtime = int8_pipeline["primary-gie"]["config-file"].as<std::string>();
  if (fs::is_regular_file(int8_runtime)) {
    const YAML::Node int8_cached = YAML::LoadFile(int8_runtime.string());
    ok &= expect(
        fs::path(int8_cached["property"]["model-engine-file"].as<std::string>()).filename() ==
            "detector_NVIDIA_Test_GPU.onnx_b2_gpu0_int8.engine",
        "cached INT8 engine path must preserve the configured network mode");
    ok &= expect(
        int8_cached["property"]["int8-calib-file"].as<std::string>() == (models / "detector_int8_calib.table").string(),
        "cached INT8 runtime config must preserve the calibration table path");
  }
  hm::pipeline::ReleaseTensorRtModelCacheLocks();
  YAML::Node missing_int8_calib_pipeline = pipeline_for("missing-int8-calib.yaml");
  ok &= expect(
      !prepare_cache(missing_int8_calib_pipeline, configs).ok(),
      "a missing INT8 calibration table must fail before DeepStream inference setup");

  YAML::Node linked_pipeline = pipeline_for("linked.yaml");
  ok &= expect(
      !prepare_cache(linked_pipeline, configs).ok(),
      "packaged ONNX symlinks must be rejected instead of reproduced in the cache");

  YAML::Node pipeline = pipeline_for("infer.yaml");
  const auto status = prepare_cache(pipeline, configs);
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
        cached_engine.filename() == "detector_NVIDIA_Test_GPU.onnx_b2_gpu0_fp32.engine",
        "engine path must use DeepStream's ONNX-derived filename rather than the configured seed name");
    ok &= expect(
        cached["property"]["labelfile-path"].as<std::string>() == (models / "labels.txt").string(),
        "relative label path must remain valid after moving the runtime config");
    ok &= expect(
        cached["property"]["custom-lib-path"].as<std::string>() == (models / "custom.so").string(),
        "relative custom library path must remain valid after moving the runtime config");
    ok &= expect(
        cached["property"]["custom-network-config"].as<std::string>() == (models / "network.cfg").string(),
        "custom network config must remain valid after moving the runtime config");
    ok &= expect(
        cached["property"]["tlt-encoded-model"].as<std::string>() == (models / "detector.etlt").string(),
        "TLT model must remain valid after moving the runtime config");
    ok &= expect(
        cached["property"]["op-tensor-files"].as<std::string>() ==
            (models / "output-0.tensor").string() + ";" + (models / "output-1.tensor").string(),
        "each output tensor path must remain valid after moving the runtime config");
    ok &= expect(
        cached["property"]["raw-output-file-write"].as<bool>(),
        "raw-output-file-write must remain a boolean instead of being treated as a path");
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

  std::ofstream(models / "output-0.tensor", std::ios::trunc) << "changed output tensor zero\n";
  YAML::Node changed_first_tensor_pipeline = pipeline_for("infer.yaml");
  ok &= expect(
      prepare_cache(changed_first_tensor_pipeline, configs).ok(),
      "a changed first output tensor must prepare successfully");
  const fs::path changed_first_tensor_runtime =
      changed_first_tensor_pipeline["primary-gie"]["config-file"].as<std::string>();
  ok &= expect(
      changed_first_tensor_runtime.parent_path() != runtime_config.parent_path(),
      "the first output tensor contents must participate in engine cache identity");
  hm::pipeline::ReleaseTensorRtModelCacheLocks();

  std::ofstream(models / "output-1.tensor", std::ios::trunc) << "changed output tensor one\n";
  YAML::Node changed_second_tensor_pipeline = pipeline_for("infer.yaml");
  ok &= expect(
      prepare_cache(changed_second_tensor_pipeline, configs).ok(),
      "a changed second output tensor must prepare successfully");
  const fs::path changed_second_tensor_runtime =
      changed_second_tensor_pipeline["primary-gie"]["config-file"].as<std::string>();
  ok &= expect(
      changed_second_tensor_runtime.parent_path() != changed_first_tensor_runtime.parent_path(),
      "the second output tensor contents must independently participate in engine cache identity");
  hm::pipeline::ReleaseTensorRtModelCacheLocks();

  std::ofstream(models / "network.cfg", std::ios::trunc) << "changed network config\n";
  YAML::Node changed_network_config_pipeline = pipeline_for("infer.yaml");
  ok &= expect(
      prepare_cache(changed_network_config_pipeline, configs).ok(),
      "a changed custom network config must prepare successfully");
  const fs::path changed_network_config_runtime =
      changed_network_config_pipeline["primary-gie"]["config-file"].as<std::string>();
  ok &= expect(
      changed_network_config_runtime.parent_path() != changed_second_tensor_runtime.parent_path(),
      "custom network config contents must participate in engine cache identity");
  hm::pipeline::ReleaseTensorRtModelCacheLocks();

  YAML::Node loader_pipeline = pipeline_for("loader.yaml");
  const fs::path parser_binary = root / "libnvdsinfer_custom_impl_Yolo.so";
  std::ofstream(parser_binary) << "staged test parser\n";
  const fs::path runtime_libraries = root / "runtime-libraries";
  fs::create_directories(runtime_libraries);
  const fs::path staged_yolo = runtime_libraries / "libnvdsinfer_custom_impl_Yolo.so";
  fs::create_symlink(parser_binary, staged_yolo);
  ::setenv("HSTREAM_NVINFER_CUSTOM_LIBRARY_DIR", runtime_libraries.c_str(), 1);
  ok &= expect(
      prepare_cache(loader_pipeline, configs).ok(),
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
      prepare_cache(writable_loader_pipeline, configs).ok(),
      "a staged custom library must prepare even when the ONNX model directory is writable");
  const fs::path writable_loader_runtime = writable_loader_pipeline["primary-gie"]["config-file"].as<std::string>();
  ok &= expect(
      writable_loader_runtime != configs / "loader-writable.yaml" && fs::is_regular_file(writable_loader_runtime),
      "a writable model must still receive a runtime inference config for its staged custom library");
  if (fs::is_regular_file(writable_loader_runtime)) {
    const YAML::Node writable_loader_cached = YAML::LoadFile(writable_loader_runtime.string());
    const fs::path writable_cached_onnx = writable_loader_cached["property"]["onnx-file"].as<std::string>();
    const fs::path writable_cached_engine = writable_loader_cached["property"]["model-engine-file"].as<std::string>();
    ok &= expect(
        writable_loader_cached["property"]["custom-lib-path"].as<std::string>() == staged_yolo.string(),
        "the writable-model runtime config must use the staged custom library path");
    ok &= expect(
        writable_cached_onnx.parent_path().parent_path() == cache &&
            fs::equivalent(writable_cached_onnx, configs / "writable-detector.onnx"),
        "writable models must also be staged in the shared engine cache");
    ok &= expect(
        writable_cached_engine == fs::path(writable_cached_onnx.string() + "_b2_gpu0_fp32.engine"),
        "writable models must load the same engine path that DeepStream serializes");
    ok &= expect(
        writable_loader_cached["property"]["custom-network-config"].as<std::string>() ==
            (models / "network.cfg").string(),
        "parser-only staging must preserve a relative custom network config");
    ok &= expect(
        writable_loader_cached["property"]["tlt-encoded-model"].as<std::string>() ==
            (models / "detector.etlt").string(),
        "parser-only staging must preserve a relative TLT model");
    ok &= expect(
        writable_loader_cached["property"]["op-tensor-files"].as<std::string>() ==
            (models / "output-0.tensor").string() + ";" + (models / "output-1.tensor").string(),
        "parser-only staging must relocate every output tensor path");
    ok &= expect(
        writable_loader_cached["property"]["raw-output-file-write"].as<bool>(),
        "parser-only staging must preserve raw-output-file-write as a boolean");
  }

  const fs::path second_runtime_libraries = root / "runtime-libraries-second-launch";
  fs::create_directories(second_runtime_libraries);
  const fs::path second_staged_yolo = second_runtime_libraries / "libnvdsinfer_custom_impl_Yolo.so";
  fs::create_symlink(parser_binary, second_staged_yolo);
  ::setenv("HSTREAM_NVINFER_CUSTOM_LIBRARY_DIR", second_runtime_libraries.c_str(), 1);
  YAML::Node second_loader_pipeline = pipeline_for("loader.yaml");
  ok &= expect(
      prepare_cache(second_loader_pipeline, configs).ok(),
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
      prepare_cache(changed_parser_pipeline, configs).ok(),
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
  ok &=
      expect(prepare_cache(changed_pipeline, configs).ok(), "changed inference build config must prepare successfully");
  const fs::path changed_runtime = changed_pipeline["primary-gie"]["config-file"].as<std::string>();
  ok &= expect(
      changed_runtime.parent_path() != fp32_runtime_directory,
      "same-path inference build changes must select a fresh engine cache identity");
  if (fs::is_regular_file(changed_runtime)) {
    const YAML::Node changed = YAML::LoadFile(changed_runtime.string());
    ok &= expect(
        fs::path(changed["property"]["model-engine-file"].as<std::string>()).filename() ==
            "detector_NVIDIA_Test_GPU.onnx_b2_gpu0_fp16.engine",
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
      prepare_cache(overridden_pipeline, configs).ok(),
      "application and section-level GIE overrides must prepare successfully");
  const fs::path overridden_runtime = overridden_pipeline["primary-gie"]["config-file"].as<std::string>();
  if (fs::is_regular_file(overridden_runtime)) {
    const YAML::Node overridden = YAML::LoadFile(overridden_runtime.string());
    const fs::path effective_engine = overridden["property"]["model-engine-file"].as<std::string>();
    ok &= expect(
        effective_engine.filename() == "detector_NVIDIA_Test_GPU_1.onnx_b4_gpu1_fp32.engine",
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
      prepare_cache(secondary_pipeline, configs).ok(), "secondary GIE application overrides must prepare successfully");
  const fs::path secondary_runtime = secondary_pipeline["secondary-gie0"]["config-file"].as<std::string>();
  if (fs::is_regular_file(secondary_runtime)) {
    const YAML::Node secondary = YAML::LoadFile(secondary_runtime.string());
    ok &= expect(
        fs::path(secondary["property"]["model-engine-file"].as<std::string>()).filename() ==
            "detector_NVIDIA_Test_GPU_2.onnx_b8_gpu2_fp32.engine",
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
      prepare_cache(primary_omitted_batch, configs).ok(),
      "primary GIE without a section batch override must prepare successfully");
  const fs::path primary_omitted_runtime = primary_omitted_batch["primary-gie"]["config-file"].as<std::string>();
  if (fs::is_regular_file(primary_omitted_runtime)) {
    const YAML::Node primary_omitted = YAML::LoadFile(primary_omitted_runtime.string());
    ok &= expect(
        fs::path(primary_omitted["property"]["model-engine-file"].as<std::string>()).filename() ==
            "detector_NVIDIA_Test_GPU.onnx_b2_gpu0_fp32.engine",
        "nvmultiurisrc must retain the inference batch when the primary section does not enable its override");
  }
  hm::pipeline::ReleaseTensorRtModelCacheLocks();

  YAML::Node secondary_omitted_batch;
  secondary_omitted_batch["application"]["use-nvmultiurisrcbin"] = 1;
  secondary_omitted_batch["application"]["sgie-batch-size"] = 8;
  secondary_omitted_batch["secondary-gie0"]["enable"] = 1;
  secondary_omitted_batch["secondary-gie0"]["config-file"] = "infer.yaml";
  ok &= expect(
      prepare_cache(secondary_omitted_batch, configs).ok(),
      "secondary GIE without a section batch override must prepare successfully");
  const fs::path secondary_omitted_runtime = secondary_omitted_batch["secondary-gie0"]["config-file"].as<std::string>();
  if (fs::is_regular_file(secondary_omitted_runtime)) {
    const YAML::Node secondary_omitted = YAML::LoadFile(secondary_omitted_runtime.string());
    ok &= expect(
        fs::path(secondary_omitted["property"]["model-engine-file"].as<std::string>()).filename() ==
            "detector_NVIDIA_Test_GPU.onnx_b2_gpu0_fp32.engine",
        "nvmultiurisrc must retain the inference batch when a secondary section does not enable its override");
  }
  hm::pipeline::ReleaseTensorRtModelCacheLocks();

  YAML::Node disabled;
  disabled["primary-gie"]["enable"] = 0;
  disabled["primary-gie"]["config-file"] = "infer.yaml";
  ok &= expect(prepare_cache(disabled, configs).ok(), "disabled inference section must be ignored");
  ok &= expect(
      disabled["primary-gie"]["config-file"].as<std::string>() == "infer.yaml",
      "disabled inference config must not be rewritten");

  fs::permissions(models, fs::perms::owner_all);
  YAML::Node writable;
  writable["primary-gie"]["enable"] = 1;
  writable["primary-gie"]["config-file"] = "infer.yaml";
  ok &= expect(prepare_cache(writable, configs).ok(), "writable development model directory must remain supported");
  ok &= expect(
      writable["primary-gie"]["config-file"].as<std::string>() == changed_network_config_runtime.string(),
      "model directory writability must not change the shared engine cache identity");
  hm::pipeline::ReleaseTensorRtModelCacheLocks();

  YAML::Node writable_linked = pipeline_for("linked.yaml");
  ok &= expect(prepare_cache(writable_linked, configs).ok(), "writable ONNX aliases must remain supported");
  const YAML::Node linked_config = YAML::LoadFile(writable_linked["primary-gie"]["config-file"].as<std::string>());
  const fs::path linked_onnx = linked_config["property"]["onnx-file"].as<std::string>();
  const fs::path linked_engine = linked_config["property"]["model-engine-file"].as<std::string>();
  ok &= expect(
      fs::is_regular_file(fs::symlink_status(linked_onnx)) && fs::equivalent(linked_onnx, models / "detector.onnx") &&
          linked_onnx.parent_path() == linked_engine.parent_path(),
      "a writable alias must stage a regular file, keeping DeepStream serialization in the cache");
  std::ofstream(linked_engine) << "cached alias engine";
  hm::pipeline::ReleaseTensorRtModelCacheLocks();
  YAML::Node warm_linked = pipeline_for("linked.yaml");
  ok &= expect(prepare_cache(warm_linked, configs).ok(), "warm ONNX alias must prepare");
  const YAML::Node warm_linked_config = YAML::LoadFile(warm_linked["primary-gie"]["config-file"].as<std::string>());
  ok &= expect(
      warm_linked_config["property"]["model-engine-file"].as<std::string>() == linked_engine.string(),
      "writable ONNX aliases must reuse the persisted engine");
  hm::pipeline::ReleaseTensorRtModelCacheLocks();

  // External tensors can occur in initializers, nested graphs and attributes.
  const fs::path external_model = configs / "external.onnx";
  const std::string tensor = external_tensor("weights/data.bin");
  const std::string nested_graph = protobuf_bytes(5, tensor);
  const std::string attributes =
      protobuf_bytes(6, nested_graph) + protobuf_bytes(10, tensor) + protobuf_bytes(22, protobuf_bytes(1, tensor));
  const std::string graph = protobuf_bytes(5, tensor) + protobuf_bytes(1, protobuf_bytes(5, attributes)) +
      protobuf_bytes(5, protobuf_bytes(9, external_tensor("not-a-real-dependency")));
  write_model(external_model, "external weights model", graph);
  const auto dependencies = hm::pipeline::OnnxExternalDataFiles(external_model);
  ok &= expect(
      dependencies.ok() && dependencies->size() == 1 && dependencies->front() == "weights/data.bin",
      "external tensor discovery must traverse nested metadata, deduplicate files, and skip raw tensor bytes");
  fs::create_directories(configs / "weights");
  std::ofstream(configs / "weights/data.bin", std::ios::binary) << "initial external weights";
  std::ofstream(configs / "external.yaml") << "property:\n  onnx-file: external.onnx\n"
                                              "  model-engine-file: missing-external.engine\n";
  YAML::Node external_pipeline = pipeline_for("external.yaml");
  ok &= expect(
      prepare_cache(external_pipeline, configs).ok(), "ONNX models with external weights must prepare successfully");
  const YAML::Node external_config = YAML::LoadFile(external_pipeline["primary-gie"]["config-file"].as<std::string>());
  const fs::path cached_external_model = external_config["property"]["onnx-file"].as<std::string>();
  const fs::path external_engine = external_config["property"]["model-engine-file"].as<std::string>();
  ok &= expect(
      fs::is_regular_file(fs::symlink_status(cached_external_model.parent_path() / "weights/data.bin")) &&
          fs::equivalent(cached_external_model.parent_path() / "weights/data.bin", configs / "weights/data.bin"),
      "the cache must preserve the external tensor's relative location with a regular file");
  std::ofstream(external_engine) << "cached external engine";
  hm::pipeline::ReleaseTensorRtModelCacheLocks();
  YAML::Node warm_external = pipeline_for("external.yaml");
  ok &= expect(prepare_cache(warm_external, configs).ok(), "warm external model must prepare");
  const YAML::Node warm_external_config = YAML::LoadFile(warm_external["primary-gie"]["config-file"].as<std::string>());
  ok &= expect(
      warm_external_config["property"]["model-engine-file"].as<std::string>() == external_engine.string(),
      "unchanged external weights must reuse the persisted engine");
  hm::pipeline::ReleaseTensorRtModelCacheLocks();
  std::ofstream(configs / "weights/data.bin", std::ios::binary) << "updated external weights";
  YAML::Node changed_external = pipeline_for("external.yaml");
  ok &= expect(prepare_cache(changed_external, configs).ok(), "changed external model must prepare");
  const YAML::Node changed_external_config =
      YAML::LoadFile(changed_external["primary-gie"]["config-file"].as<std::string>());
  ok &= expect(
      changed_external_config["property"]["model-engine-file"].as<std::string>() != external_engine.string(),
      "external weight contents must invalidate the engine even when ONNX bytes are unchanged");
  hm::pipeline::ReleaseTensorRtModelCacheLocks();
  fs::remove(configs / "weights/data.bin");
  YAML::Node missing_external = pipeline_for("external.yaml");
  ok &= expect(
      !prepare_cache(missing_external, configs).ok(),
      "missing external weights must fail before publishing an incomplete model");
  for (const char* unsafe_location : {"../weights.bin", "/absolute/weights.bin", ""}) {
    write_model(external_model, "unsafe external weights", protobuf_bytes(5, external_tensor(unsafe_location)));
    ok &= expect(
        !hm::pipeline::OnnxExternalDataFiles(external_model).ok(),
        "external weights must not escape the model cache directory");
  }
  std::ofstream(external_model, std::ios::binary) << '\x3a' << '\x7f' << 'x';
  ok &= expect(!hm::pipeline::OnnxExternalDataFiles(external_model).ok(), "truncated ONNX metadata must be rejected");

  // Reproduce the default detector setup: the ONNX is writable under HOME,
  // while the configured engine seed differs from DeepStream's output path.
  fs::remove(home_models / "home-detector.engine");
  YAML::Node cold_pipeline = pipeline_for("home-dollar.yaml");
  ok &= expect(
      prepare_cache(cold_pipeline, configs).ok(),
      "a writable user model with a missing engine must use the shared cache");
  const fs::path cold_runtime = cold_pipeline["primary-gie"]["config-file"].as<std::string>();
  const YAML::Node cold_config = YAML::LoadFile(cold_runtime.string());
  const fs::path cold_onnx = cold_config["property"]["onnx-file"].as<std::string>();
  const fs::path cold_engine = cold_config["property"]["model-engine-file"].as<std::string>();
  const fs::path serialized_engine = cold_onnx.string() + "_b1_gpu0_fp32.engine";
  ok &= expect(
      cold_engine == serialized_engine && cold_engine.parent_path().parent_path() == cache,
      "generated engine lookup and serialization must agree inside the shared cache");
  std::ofstream(serialized_engine) << "engine serialized by DeepStream\n";
  const auto engine_write_time = fs::last_write_time(serialized_engine);
  hm::pipeline::ReleaseTensorRtModelCacheLocks();

  YAML::Node warm_pipeline = pipeline_for("home-dollar.yaml");
  warm_pipeline["source0"]["uri"] = "file:///different-game/cam1.mp4";
  warm_pipeline["sink0"]["output-file"] = "/different-output/game.mkv";
  ok &= expect(prepare_cache(warm_pipeline, configs).ok(), "a later run for another game must prepare successfully");
  const YAML::Node warm_config = YAML::LoadFile(warm_pipeline["primary-gie"]["config-file"].as<std::string>());
  ok &= expect(
      warm_config["property"]["model-engine-file"].as<std::string>() == serialized_engine.string() &&
          fs::last_write_time(serialized_engine) == engine_write_time && fs::file_size(serialized_engine) != 0,
      "a later run must reuse the persisted engine without touching it");
  ok &= expect(
      !fs::exists(home_models / "home-detector.onnx_b1_gpu0_fp32.engine"),
      "engine generation must not write beside the source ONNX model");
  hm::pipeline::ReleaseTensorRtModelCacheLocks();

  // Reusing ordinal zero on a different GPU must neither load nor overwrite
  // the first device's engine or runtime YAML.
  write_inference_config(configs / "gpu-cache.yaml", 0);
  auto gpu_first = pipeline_for("gpu-cache.yaml");
  ok &= expect(prepare_cache(gpu_first, configs).ok(), "first GPU cache must prepare");
  const auto first_config = gpu_first["primary-gie"]["config-file"].as<std::string>();
  const auto first_engine = YAML::LoadFile(first_config)["property"]["model-engine-file"].as<std::string>();
  std::ofstream(first_engine) << "first GPU engine";
  hm::pipeline::ReleaseTensorRtModelCacheLocks();
  gpu_model = "NVIDIA_Other_GPU";
  auto gpu_second = pipeline_for("gpu-cache.yaml");
  ok &= expect(prepare_cache(gpu_second, configs).ok(), "second GPU cache must prepare");
  const auto second_config = gpu_second["primary-gie"]["config-file"].as<std::string>();
  const auto second_engine = YAML::LoadFile(second_config)["property"]["model-engine-file"].as<std::string>();
  ok &= expect(
      first_config != second_config && first_engine != second_engine && !fs::exists(second_engine) &&
          fs::file_size(first_engine) == 16,
      "GPU models must have independent engine and runtime config paths");
  hm::pipeline::ReleaseTensorRtModelCacheLocks();
  for (const char* model : {"NVIDIA_Test_GPU", "NVIDIA_Other_GPU"}) {
    const auto engine = home_models / (std::string("prepared_") + model + "_int8.engine");
    std::ofstream(engine) << model;
  }
  std::ofstream(configs / "gpu-prebuilt.yaml") << "hstream-prebuilt-precision: int8\nproperty:\n  model-engine-file: "
                                               << (home_models / "prepared_{gpu}_int8.engine").string() << '\n';
  for (const char* model : {"NVIDIA_Test_GPU", "NVIDIA_Other_GPU"}) {
    gpu_model = model;
    auto selected = pipeline_for("gpu-prebuilt.yaml");
    ok &= expect(prepare_cache(selected, configs).ok(), "prebuilt GPU template must resolve");
    const auto prepared = YAML::LoadFile(selected["primary-gie"]["config-file"].as<std::string>());
    ok &= expect(
        prepared["property"]["model-engine-file"].as<std::string>() ==
            (home_models / (std::string("prepared_") + model + "_int8.engine")).string(),
        "prebuilt selection must use the actual GPU model");
  }
  gpu_model = "NVIDIA_Test_GPU";
  ok &= expect(
      hm::inference::SanitizeGpuName("NVIDIA GeForce RTX-5090") == "NVIDIA_GeForce_RTX_5090",
      "GPU names must be safe filename components");

  ::unsetenv("HSTREAM_TENSORRT_CACHE_DIR");
  for (bool use_xdg : {true, false}) {
    const fs::path xdg_cache = root / "xdg-cache";
    if (use_xdg)
      ::setenv("XDG_CACHE_HOME", xdg_cache.c_str(), 1);
    else
      ::unsetenv("XDG_CACHE_HOME");
    YAML::Node default_pipeline = pipeline_for("home-dollar.yaml");
    ok &= expect(
        prepare_cache(default_pipeline, configs).ok(), "the default shared engine cache must prepare successfully");
    const fs::path default_runtime = default_pipeline["primary-gie"]["config-file"].as<std::string>();
    ok &= expect(
        default_runtime.parent_path().parent_path() == (use_xdg ? xdg_cache : home / ".cache") / "hstream/tensorrt",
        "the default cache must honor XDG_CACHE_HOME and fall back to ~/.cache/hstream/tensorrt");
    hm::pipeline::ReleaseTensorRtModelCacheLocks();
  }
  hm::pipeline::ReleaseTensorRtModelCacheLocks();
  fs::remove_all(root);
  return ok ? 0 : 1;
}
