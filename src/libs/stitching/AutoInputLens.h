#pragma once

#include "hstream/src/libs/stitching/VideoLensMetadata.h"

namespace hm::stitching {
// Resolve the selected source playlists without decoding for orientation. Only
// Auto selections are imported; experiment snapshots can freeze exact values.
AutoInputLensResult ReadAutoInputLensForGame(
    const std::filesystem::path& game,
    const YAML::Node& config,
    const StitchCameraSelection& selection,
    bool force_video_discovery = false);
} // namespace hm::stitching
