#pragma once

#include "hstream/src/libs/stitching/ConfigureStitchingCore.h"
#include "hstream/src/libs/stitching/Synchronization.h"

namespace hm::stitching {
// Plan a circular, evenly spaced sample on the common synchronized clock.
// The first pair is always the requested reference. Inputs are already trimmed
// by each camera's effective synchronization offset.
absl::StatusOr<std::vector<std::array<CalibrationFrameSource, 2>>> PlanCalibrationReferenceFrames(
    const std::array<CameraRecordingWindow, 2>& cameras,
    int64_t reference_ns,
    size_t count);

// Header probes only; image writers seek/decode lazily after core generation
// preflight. Actual decoded PTS replaces the requested timestamp in provenance.
absl::StatusOr<std::vector<CalibrationFrameInput>> EndRelativeCalibrationInputs(
    const std::filesystem::path& game,
    const YAML::Node& config,
    size_t count,
    const std::function<bool()>& is_cancelled);
} // namespace hm::stitching
