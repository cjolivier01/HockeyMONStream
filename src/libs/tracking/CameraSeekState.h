#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "hockeymon/csrc/play_tracker/PlayTrackerSnapshot.h"

namespace hm::tracking {
// Private, metadata-only handoff between a retiring vpplaytracker and its
// replacement. The old element owns capture through worker shutdown; the new
// element retains it through caps and positioning flushes until first output.
inline constexpr char kCaptureCameraSeekState[] = "hstream-capture-camera-seek-state";
inline constexpr char kCameraSeekState[] = "hstream-camera-seek-state";
// Match the established native snapshot parser's persisted-input ceiling.
inline constexpr size_t kMaximumCameraSeekStateBytes = 16 * 1024 * 1024;

struct CameraSeekState {
  std::string checkpoint;
  absl::Status status;
  bool has_received_tracks{false};
  uint64_t seek_generation{0};
};

absl::StatusOr<std::string> CaptureCameraSeekState(const play_tracker::PlayTracker& tracker);
// Recorded dynamics use current tuning. A full rebuild instead retains the
// captured live tuning, which must apply before preroll and property replay.
// Validation finishes before the caller replaces any live state.
absl::StatusOr<play_tracker::PlayTrackerSnapshot> PrepareCameraSeekState(
    const std::string& checkpoint,
    const play_tracker::PlayTrackerSnapshot& current,
    bool retain_checkpoint_tuning = false);
} // namespace hm::tracking
