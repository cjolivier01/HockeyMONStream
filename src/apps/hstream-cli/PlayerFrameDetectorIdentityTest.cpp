#include "PlayerFrameDetectorIdentity.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>

#include <yaml-cpp/yaml.h>

namespace fs = std::filesystem;
namespace {
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}
void write_config(const fs::path& path, const YAML::Node& config) {
  std::ofstream(path) << config;
}
} // namespace

int main() {
  using namespace hm::pipeline;
  bool ok = true;
  const fs::path directory = fs::temp_directory_path() / ("player-detector-identity-" + std::to_string(getpid()));
  fs::create_directories(directory);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code e;
      fs::remove_all(path, e);
    }
  } cleanup{directory};
  const fs::path config_path = directory / "inference.yaml";
  const fs::path model_path = directory / "model.onnx";
  const fs::path engine_path = directory / "generated.engine";
  const fs::path labels = directory / "labels.txt";
  // Valid protobuf metadata containing ModelProto.ir_version = 8; no tensor payloads.
  std::ofstream(model_path, std::ios::binary).write("\x08\x08", 2);
  std::ofstream(labels) << "person\n";
  YAML::Node config;
  config["property"]["onnx-file"] = "model.onnx";
  config["property"]["model-engine-file"] = "generated.engine";
  config["property"]["labelfile-path"] = "labels.txt";
  // Exercise actual dynamic-loader basename resolution with an installed library.
  config["property"]["custom-lib-path"] = "libm.so.6";
  write_config(config_path, config);
  auto before_engine = PlayerFrameDetectorModelIdentity(config_path, 1);
  ok &= expect(before_engine.ok(), "semantic model identity is available before expected engine construction");
  if (!before_engine.ok()) {
    std::cerr << before_engine.status() << '\n';
    return 1;
  }
  ok &= expect(
      !PlayerFrameDetectorRuntimeIdentity(config_path, engine_path).ok(),
      "runtime binding requires an actual engine file");
  std::ofstream(engine_path) << "engine-generated-at-preroll";
  auto after_engine = PlayerFrameDetectorModelIdentity(config_path, 1);
  auto runtime = PlayerFrameDetectorRuntimeIdentity(config_path, engine_path);
  ok &= expect(
      after_engine.ok() && *after_engine == *before_engine && runtime.ok(),
      "expected engine creation preserves model identity and supplies runtime identity");
  std::ofstream(engine_path, std::ios::app) << "changed";
  auto changed_runtime = PlayerFrameDetectorRuntimeIdentity(config_path, engine_path);
  ok &= expect(
      runtime.ok() && changed_runtime.ok() && *runtime != *changed_runtime,
      "engine bytes are bound even when ONNX remains unchanged");
  auto after_changed_engine = PlayerFrameDetectorModelIdentity(config_path, 1);
  ok &= expect(
      after_changed_engine.ok() && *after_changed_engine == *before_engine,
      "engine mutation belongs to runtime verification, not expected engine creation semantics");
  std::ofstream(labels) << "observer\n";
  auto changed_labels = PlayerFrameDetectorModelIdentity(config_path, 1);
  ok &= expect(
      changed_labels.ok() && *changed_labels != *before_engine, "label content participates in semantic identity");
  config["property"]["custom-lib-path"] = "hstream-nonexistent-parser-fixture.so";
  write_config(config_path, config);
  ok &= expect(
      !PlayerFrameDetectorModelIdentity(config_path, 1).ok() &&
          !PlayerFrameDetectorRuntimeIdentity(config_path, engine_path).ok(),
      "unresolved parser basenames fail instead of silently dropping identity");
  config["property"]["custom-lib-path"] = (directory / "absent-parser.so").string();
  write_config(config_path, config);
  ok &= expect(!PlayerFrameDetectorModelIdentity(config_path, 1).ok(), "missing explicit parser paths fail");
  config["property"].remove("custom-lib-path");
  config["property"].remove("onnx-file");
  write_config(config_path, config);
  auto engine_only = PlayerFrameDetectorModelIdentity(config_path, 1);
  std::ofstream(engine_path, std::ios::app) << "another-change";
  auto engine_only_changed = PlayerFrameDetectorModelIdentity(config_path, 1);
  ok &= expect(
      engine_only.ok() && engine_only_changed.ok() && *engine_only != *engine_only_changed,
      "engine-only detectors bind existing engine bytes during preparation");
  fs::remove(labels);
  ok &= expect(!PlayerFrameDetectorModelIdentity(config_path, 1).ok(), "missing configured labels fail");
  return ok ? 0 : 1;
}
