#include "hstream/src/libs/tracker_reid/ReIdConfig.h"

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "yaml-cpp/yaml.h"

namespace {
namespace fs = std::filesystem;
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}
void write(const fs::path& path, const std::string& text) {
  std::ofstream(path) << text;
}
const char* kOverlay = R"YAML(
ReID:
  modelEngineFile: engines/fixture.engine
  batchSize: 8
  reidFeatureSize: 256
  reidHistorySize: 32
  inferDims: [3, 256, 128]
  networkMode: 1
  inputOrder: 0
  colorFormat: 0
  offsets: [123.675, 116.28, 103.53]
  netScaleFactor: 0.01735207357279195
  keepAspc: 0
  addFeatureNormalization: 1
TrajectoryManagement:
  reidExtractionInterval: 7
)YAML";
} // namespace

int main() {
  bool ok = true;
  hm::tracker_reid::Options options;
  options.base_config = "/cannot/exist/base.yml";
  options.overlay_config = "/cannot/exist/reid.yml";
  options.sub_batches = "unsupported";
  auto off = hm::tracker_reid::Prepare(options);
  ok &= expect(off.ok() && !*off, "all-off ignores every path and unsupported settings");
  options.reid_enabled = true;
  off = hm::tracker_reid::Prepare(options);
  ok &= expect(off.ok() && !*off, "disabled tracker does no ReID work");
  options.reid_enabled = false;
  options.tracker_enabled = true;
  off = hm::tracker_reid::Prepare(options);
  ok &= expect(off.ok() && !*off, "disabled ReID does no work on enabled tracker");

  char pattern[] = "/tmp/hstream-reid-test-XXXXXX";
  const char* directory = ::mkdtemp(pattern);
  if (!directory)
    return 1;
  const fs::path root(directory);
  fs::create_directory(root / "engines");
  write(root / "engines/fixture.engine", "CPU test placeholder; native test uses real pretrained engine");
  write(root / "base.yml", R"YAML(%YAML:1.0
TargetManagement: {maxTargetsPerStream: 150, maxShadowTrackingAge: 51}
VisualTracker: {visualTrackerType: 1, filterLr: 0.125}
DataAssociator: {dataAssociatorType: 0}
TrajectoryManagement: {useUniqueID: 1, enableReAssoc: 0}
ReID:
  onnxFile: missing.onnx
  tltEncodedModel: old.etlt
  tltModelKey: old
  modelEngineFile: old.engine
  keepAspc: 1
  outputReidTensor: 1
)YAML");
  write(root / "overlay.yml", kOverlay);
  options = {
      true,
      true,
      "/opt/libnvds_nvmultiobjecttracker.so",
      (root / "base.yml").string() + ";",
      (root / "overlay.yml").string(),
      ""};
  auto prepared = hm::tracker_reid::Prepare(options);
  ok &= expect(prepared.ok() && *prepared, "valid overlay prepares from OpenCV-style base");
  fs::path generated;
  if (prepared.ok()) {
    generated = (*prepared)->path();
    std::ifstream stream(generated);
    std::string text((std::istreambuf_iterator<char>(stream)), {});
    const YAML::Node merged = YAML::Load(text.substr(text.find('\n') + 1));
    ok &= expect(
        merged["VisualTracker"]["filterLr"].as<double>() == 0.125 &&
            merged["TargetManagement"]["maxShadowTrackingAge"].as<int>() == 51 &&
            merged["TrajectoryManagement"]["useUniqueID"].as<int>() == 1,
        "unrelated base tracker tuning is preserved");
    ok &= expect(
        merged["TrajectoryManagement"]["enableReAssoc"].as<int>() == 1 &&
            merged["TrajectoryManagement"]["reidExtractionInterval"].as<int>() == 7,
        "reassociation and bounded cadence are set");
    const YAML::Node reid = merged["ReID"];
    ok &= expect(
        reid["modelEngineFile"].as<std::string>() == fs::canonical(root / "engines/fixture.engine") &&
            !reid["onnxFile"] && !reid["tltEncodedModel"] && !reid["tltModelKey"] &&
            reid["outputReidTensor"].as<int>() == 0 && reid["keepAspc"].as<int>() == 0,
        "original overlay engine path resolves and inherited rebuild inputs are absent");
    struct stat file_stat{}, dir_stat{};
    ::stat(generated.c_str(), &file_stat);
    ::stat(generated.parent_path().c_str(), &dir_stat);
    ok &=
        expect((file_stat.st_mode & 0777) == 0600 && (dir_stat.st_mode & 0777) == 0700, "generated files are private");
    prepared->reset();
    ok &= expect(
        !fs::exists(generated) && !fs::exists(generated.parent_path()) && fs::exists(root / "engines/fixture.engine") &&
            fs::exists(root / "base.yml"),
        "owner deletes only private runtime files");
  }

  const auto rejects = [&](const std::string& yaml, const char* message) {
    write(root / "overlay.yml", yaml);
    return expect(!hm::tracker_reid::Prepare(options).ok(), message);
  };
  ok &= rejects(std::string(kOverlay) + "\nunknown: 1\n", "unknown overlay group rejected");
  YAML::Node changed = YAML::Load(kOverlay);
  changed["ReID"]["onnxFile"] = "rebuild.onnx";
  ok &= rejects(YAML::Dump(changed), "rebuild input in overlay rejected");
  changed = YAML::Load(kOverlay);
  changed["ReID"]["modelEngineFile"] = "missing.engine";
  ok &= rejects(YAML::Dump(changed), "missing engine rejected");
  changed = YAML::Load(kOverlay);
  changed["ReID"]["netScaleFactor"] = ".nan";
  ok &= rejects(YAML::Dump(changed), "nonfinite normalization rejected");
  changed = YAML::Load(kOverlay);
  changed["ReID"]["reidHistorySize"] = 129;
  ok &= rejects(YAML::Dump(changed), "unbounded gallery rejected");
  changed = YAML::Load(kOverlay);
  changed["ReID"]["inferDims"] = YAML::Load("[3,1024,1024]");
  ok &= rejects(YAML::Dump(changed), "oversized input pool rejected");
  changed = YAML::Load(kOverlay);
  changed["TrajectoryManagement"]["reidExtractionInterval"] = 0;
  ok &= rejects(YAML::Dump(changed), "zero extraction cadence rejected");
  changed = YAML::Load(kOverlay);
  changed["ReID"].remove("keepAspc");
  ok &= rejects(YAML::Dump(changed), "preprocessing cannot inherit from unrelated base model");
  ok &= rejects("ReID: {}\nReID: {}\n", "duplicate groups rejected");
  write(root / "overlay.yml", kOverlay);
  options.sub_batches = "0";
  ok &= expect(!hm::tracker_reid::Prepare(options).ok(), "sub-batches explicitly rejected");
  options.sub_batches.clear();
  options.base_config += "other.yml";
  ok &= expect(!hm::tracker_reid::Prepare(options).ok(), "multiple low-level configs rejected");
  options.base_config = (root / "base.yml").string();
  options.tracker_library = "/opt/libcustom_tracker.so";
  ok &= expect(!hm::tracker_reid::Prepare(options).ok(), "unsupported tracker library rejected");
  options.tracker_library = "/opt/libnvds_nvmultiobjecttracker.so";
  write(root / "base.yml", "VisualTracker: {visualTrackerType: 0}\n");
  ok &= expect(!hm::tracker_reid::Prepare(options).ok(), "SORT/non-NvDCF configuration rejected");
  fs::remove_all(root);
  if (ok)
    std::cout << "ReID config tests passed\n";
  return ok ? 0 : 1;
}
