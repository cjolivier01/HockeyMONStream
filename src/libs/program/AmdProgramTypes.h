#pragma once

#include <gst/gst.h>
#include <gst/video/video-info.h>

#include <filesystem>
#include <memory>
#include <string>

#include "cupano/pano/blendMode.h"
#include "hstream/src/libs/detection/HipDetector.h"
#include "hstream/src/libs/program/ProgramCalibration.h"
#include "hstream/src/libs/program/ProgramPreviewBoxes.h"
#include "hstream/src/libs/program/ProgramRenderSettings.h"
#include "hstream/src/libs/tracking/ProgramTracking.h"

namespace hm::program {
// Fully resolved settings. This owner supplies no application defaults. The
// caller selects the same original camera sources used by the saved calibration.
struct AmdProgramProcessorConfig {
  std::filesystem::path game_directory;
  detection::DetectorConfig detector;
  YAML::Node camera;
  tracking::TrackerConfig tracker;
  pano::BlendSettings blend;
  bool minimize_blend, compact_workspace;
  float raise_center_ratio, lower_bottom_ratio;
  ProgramCalibrationRequest calibration;
};

struct AmdProgramFrame {
  size_t detection_count;
  tracking::ProgramObservation observation;
  hm::BBox crop;
};

} // namespace hm::program
