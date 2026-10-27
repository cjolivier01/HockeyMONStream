#pragma once

#include "absl/status/statusor.h"
#include "hstream/src/libs/common/PacedPlayback.h"
#include "yaml-cpp/yaml.h"

namespace hm::gst {
absl::StatusOr<PacedPlaybackSettings> ReadPacedPlaybackSettings(const YAML::Node& config);
} // namespace hm::gst
