#pragma once

#include <string>

#include <yaml-cpp/yaml.h>

namespace hm::pipeline_internal {

// Keep stitching calibration on the shortest useful video graph. Sources,
// streammux, hmstitcher, sinks, audio, and preview branches remain available;
// every stage that consumes the stitched output for Program production is
// disabled before the DeepStream graph is parsed and constructed.
inline void configure_stitching_calibration_pipeline(YAML::Node pipeline) {
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
    pipeline["hmstitcher"]["private-properties"]["calibrate-field-mask"] = 0;
}

} // namespace hm::pipeline_internal
