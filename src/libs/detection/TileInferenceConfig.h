#pragma once
#include <optional>
#include "hstream/src/libs/detection/TileInference.h"
#include "yaml-cpp/yaml.h"

namespace hm::detection {
// The caller supplies the baseline/user/game/CLI-resolved tiled-inference map.
// No independent defaults: missing or malformed configuration fails startup.
absl::StatusOr<std::optional<TileInferenceSettings>> ReadTileInferenceSettings(const YAML::Node& node);
} // namespace hm::detection
