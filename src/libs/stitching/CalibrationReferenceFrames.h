#pragma once

#include "hstream/src/libs/stitching/ConfigureStitchingCore.h"
#include "hstream/src/libs/stitching/Synchronization.h"

namespace hm::stitching {
struct CalibrationRecordingTimeline {
  uint64_t duration_ns;
  uint64_t frame_interval_ns;
};
absl::StatusOr<CalibrationRecordingTimeline> ReadCalibrationRecordingTimeline(
    const std::filesystem::path& game,
    const YAML::Node& config);
// Plan evenly spaced samples on the common synchronized clock, wrapping to
// Playback start without revisiting the tail.
// The first pair is always the requested reference. Inputs are already trimmed
// by each camera's effective synchronization offset.
absl::StatusOr<std::vector<std::array<CalibrationFrameSource, 2>>> PlanCalibrationReferenceFrames(
    const std::array<CameraRecordingWindow, 2>& cameras,
    int64_t reference_ns,
    size_t count,
    uint64_t playback_start_ns);

// Header probes only; image writers seek/decode lazily after core generation
// preflight. Actual decoded PTS replaces the requested timestamp in provenance.
absl::StatusOr<std::vector<CalibrationFrameInput>> EndRelativeCalibrationInputs(
    const std::filesystem::path& game,
    const YAML::Node& config,
    size_t count,
    const std::function<bool()>& is_cancelled,
    std::optional<uint64_t> playback_start_ns = std::nullopt);
} // namespace hm::stitching
