#pragma once

#include "hstream/src/libs/common/ConfigLayers.h"
#include "hstream/src/libs/program/ProgramRenderSettings.h"

namespace hm::program {
struct ProgramOutputConfig {
  int width, height;
  unsigned bitrate_kbps, keyframe_interval;
  bool realtime;
  uint64_t start_ns;
  ProgramRenderSettings render;
};
// Resolve AMD output defaults from the shared baseline and honor canonical
// output dimensions/bitrate plus native and UI render controls by layer rank.
absl::StatusOr<ProgramOutputConfig> ResolveProgramOutputConfig(const config::ProgramConfiguration& config);
} // namespace hm::program
