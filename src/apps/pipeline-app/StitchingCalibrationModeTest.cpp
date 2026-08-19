#include "src/apps/pipeline-app/StitchingCalibrationMode.h"
#include "src/apps/pipeline-app/StitcherOnePassConfig.h"

#include <iostream>
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

int main() {
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
  return ok ? 0 : 1;
}
