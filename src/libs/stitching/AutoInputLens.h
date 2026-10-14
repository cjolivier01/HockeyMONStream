#pragma once

#include "hstream/src/libs/stitching/VideoLensMetadata.h"

namespace hm::stitching {
// Recognized non-null optics leaves distinguish legacy manual settings from
// empty/null maps that simply inherit the selected camera profile.
bool HasManualInputLensOverrides(const YAML::Node& config);

// Resolve the selected source playlists without decoding for orientation. Only
// Auto selections are imported; experiment snapshots can freeze exact values.
AutoInputLensResult ReadAutoInputLensForGame(
    const std::filesystem::path& game,
    const YAML::Node& config,
    const StitchCameraSelection& selection,
    bool force_video_discovery = false);
} // namespace hm::stitching
