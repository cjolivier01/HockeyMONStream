#pragma once

#include <yaml-cpp/yaml.h>
#include "hstream/src/libs/scoreboard/CameraSession.h"

namespace hm::scoreboard {
// Signed HH:MM:SS.ssss; positive skips camera time, negative delays appearance.
absl::StatusOr<int64_t> ParseCameraStartOffset(const std::string& value);

// Resolve the canonical baseline + user + game camera map. Files stay outside
// the stitching input list; polygon coordinates belong to this source only.
// Playback supplies epoch/start and the live game clock before Start().
absl::StatusOr<std::optional<CameraSessionConfig>> ResolveCameraConfig(
    const YAML::Node& camera,
    const std::filesystem::path& game_directory,
    CameraDecodeBackend backend,
    unsigned device,
    const std::filesystem::path& render_node);
} // namespace hm::scoreboard
