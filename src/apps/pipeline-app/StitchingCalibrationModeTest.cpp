#include "src/apps/pipeline-app/StitchingCalibrationMode.h"
#include "hstream/src/libs/assets/AssetManager.h"
#include "hstream/src/libs/stitching/CalibrationCompletion.h"
#include "src/apps/pipeline-app/StitcherOnePassConfig.h"

#include <filesystem>
#include <iostream>
#include <set>
#include <string>

#include <yaml-cpp/yaml.h>

namespace {

bool enabled(const YAML::Node& pipeline, const std::string& section) {
  return pipeline[section] && pipeline[section]["enable"] && pipeline[section]["enable"].as<int>() != 0;
}

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

} // namespace

int main(int argc, char* argv[]) {
  YAML::Node config = YAML::Load(R"yaml(
pipeline:
  source0: {enable: 1}
  streammux: {batch-size: 2}
  hmstitcher: {enable: 1}
  primary-gie: {enable: 1}
  secondary-gie0: {enable: 1}
  secondary-preprocess0: {enable: 1}
  tracker: {enable: 1}
  ds-fieldmask: {enable: 1}
  ds-playtracker: {enable: 1}
  hmplaycropper: {enable: 1}
  nvds-analytics: {enable: 1}
  pre-process: {enable: 1}
  osd: {enable: 1}
  tiled-display: {enable: 1}
  img-save: {enable: 1}
  message-converter: {enable: 1}
  sink0: {enable: 1}
  hmaudio0: {enable: 1}
)yaml");

  YAML::Node pipeline = config["pipeline"];
  hm::pipeline_internal::configure_stitching_calibration_pipeline(pipeline);

  bool ok = true;
  ok &= expect(enabled(pipeline, "source0"), "calibration must retain decoded video sources");
  ok &= expect(enabled(pipeline, "hmstitcher"), "calibration must retain hmstitcher");
  ok &= expect(enabled(pipeline, "sink0"), "calibration must retain configured sinks");
  ok &= expect(enabled(pipeline, "hmaudio0"), "calibration must retain configured audio routing");
  ok &= expect(
      pipeline["hmstitcher"]["private-properties"]["calibrate-field-mask"].as<int>() == 0,
      "calibration must not generate the downstream rink mask");
  ok &= expect(
      !hm::StitcherCalibratesFieldMask(pipeline), "calibration completion must not wait for the omitted field mask");
  ok &= expect(
      hm::StitcherCalibratesFieldMask(YAML::Load("hmstitcher: {one-pass-mode: 1}")),
      "Program mode must retain field-mask calibration by default");
  ok &= expect(
      !hm::OnePassCalibrationRequiredForMode(true, true, false, false, false),
      "a completed stitching-only preview must not wait for a rink mask");
  ok &= expect(
      hm::OnePassCalibrationRequiredForMode(true, true, false, false, true),
      "a pending stitching-only run must still publish calibration completion");
  ok &= expect(
      hm::OnePassCalibrationRequiredForMode(true, true, false, true, false),
      "Program mode must still calibrate a missing rink mask");
  const std::string completion_scope = hm::stitching::calibration_completion_scope("output-a", "owner-a", "17");
  ok &= expect(
      completion_scope == hm::stitching::calibration_completion_scope("output-a", "owner-a", "17") &&
          completion_scope != hm::stitching::calibration_completion_scope("output-a", "owner-b", "17") &&
          completion_scope != hm::stitching::calibration_completion_scope("output-a", "owner-a", "18") &&
          completion_scope != hm::stitching::calibration_completion_scope("output-b", "owner-a", "17"),
      "completion scope must reject stale output, owner, and pipeline generations");
  for (const char* stage : {
           "primary-gie",
           "secondary-gie0",
           "secondary-preprocess0",
           "tracker",
           "ds-fieldmask",
           "ds-playtracker",
           "hmplaycropper",
           "nvds-analytics",
           "pre-process",
           "osd",
           "tiled-display",
           "img-save",
           "message-converter",
       }) {
    ok &= expect(!enabled(pipeline, stage), (std::string("calibration must disable ") + stage).c_str());
  }
  ok &= expect(argc == 2, "test must receive the hockey application config");
  if (argc == 2) {
    auto assets = hm::assets::AssetManager::Discover({std::filesystem::path(argv[1])}, [](YAML::Node config) {
      hm::pipeline_internal::configure_stitching_calibration_pipeline(config);
    });
    std::set<std::string> asset_names;
    if (assets.ok()) {
      for (const auto& asset : *assets)
        asset_names.insert(asset.name);
    }
    ok &= expect(
        assets.ok() && assets->size() == 4 && asset_names.count("ice-rink-mask2former-swin-s") == 1 &&
            asset_names.count("superpoint-lightglue") == 1 && asset_names.count("dedode-lightglue") == 1 &&
            asset_names.count("efficient-loftr-outdoor") == 1,
        "real calibration discovery must retain orientation/matching models and omit Program detector assets");
  }
  return ok ? 0 : 1;
}
