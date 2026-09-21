#pragma once

#include <string>

#include <yaml-cpp/yaml.h>

namespace hm::pipeline_internal {

// Keep stitching calibration on the shortest useful video graph. Sources,
// streammux, hmstitcher, sinks, audio, and preview branches remain available;
// every stage that consumes the stitched output for Program production is
// disabled before the DeepStream graph is parsed and constructed.
inline void configure_stitching_calibration_pipeline(YAML::Node pipeline, bool prepare_ice_mask = false) {
  if (!pipeline || !pipeline.IsMap())
    return;

  constexpr const char* kDownstreamStages[] = {
      "hm-image-meta-merger",
      "hmplaycropper",
      "segvisual",
      "primary-gie",
      "nvds-analytics",
      "ds-playtracker",
      "tracker",
      "ds-example",
      "ds-fieldmask",
      "pre-process",
      "osd",
      "tiled-display",
      "img-save",
      "message-converter",
  };
  for (const char* stage : kDownstreamStages) {
    if (pipeline[stage].IsDefined())
      pipeline[stage]["enable"] = 0;
  }

  for (const auto& entry : pipeline) {
    if (!entry.first.IsScalar() || !entry.second.IsMap())
      continue;
    const std::string name = entry.first.as<std::string>();
    if (name.rfind("secondary-gie", 0) == 0 || name.rfind("secondary-preprocess", 0) == 0)
      pipeline[name]["enable"] = 0;
  }

  // The explicit calibration UI only needs stitching artifacts. Program mode
  // retains the default and generates the rink mask before continuing into
  // detection and play tracking.
  if (pipeline["hmstitcher"].IsMap())
    pipeline["hmstitcher"]["private-properties"]["calibrate-field-mask"] = prepare_ice_mask ? 1 : 0;
}

// Player-frame analysis consumes the exact rink-filtered population that feeds
// Program tracking. This policy also runs during asset discovery, before model
// resolution, so enabling inference here provisions the normal detector.
inline void configure_stitching_player_scan_pipeline(YAML::Node pipeline) {
  if (!pipeline || !pipeline.IsMap())
    return;
  configure_stitching_calibration_pipeline(pipeline);
  pipeline["primary-gie"]["enable"] = 1;
  pipeline["primary-gie"]["interval"] = 0;
  pipeline["ds-fieldmask"]["enable"] = 1;
  pipeline["ds-fieldmask"]["require-existing-mask"] = 1;
  pipeline["ds-fieldmask"]["properties"]["require-existing-mask"] = true;
  pipeline["hmstitcher"]["private-properties"]["emit-frame-pair-meta"] = 1;
  pipeline["hmstitcher"]["show"] = 0;
  // One synchronized camera pair yields one stitched frame. The scan gate can
  // discard unsampled buffers without altering any source/mux completeness rule.
  pipeline["streammux"]["batch-size"] = 2;
  for (const auto& entry : pipeline) {
    if (!entry.first.IsScalar() || !entry.second.IsMap())
      continue;
    const std::string name = entry.first.as<std::string>();
    if (name.rfind("hmaudio", 0) == 0 || name.rfind("sink", 0) == 0)
      pipeline[name]["enable"] = 0;
  }
  pipeline["sink0"]["enable"] = 1;
  pipeline["sink0"]["type"] = 1;
  pipeline["sink0"]["sync"] = 0;
  pipeline["sink0"]["sink-id"] = 0;
  pipeline["tests"]["pipeline-recreate-sec"] = 0;
  pipeline["tests"]["file-loop"] = 0;
}

} // namespace hm::pipeline_internal
