#include "hstream/src/libs/tracking/CameraSeekState.h"

#include <exception>

namespace hm::tracking {
absl::StatusOr<std::string> CaptureCameraSeekState(const play_tracker::PlayTracker& tracker) {
  try {
    auto state = tracker.snapshot();
    state.detector.tracks.clear();
    auto serialized = play_tracker::serialize_snapshot(state);
    if (serialized.size() > kMaximumCameraSeekStateBytes)
      return absl::ResourceExhaustedError("Camera seek checkpoint exceeds its metadata bound");
    return serialized;
  } catch (const std::exception& error) {
    return absl::FailedPreconditionError(std::string("Cannot capture camera seek state: ") + error.what());
  }
}

absl::StatusOr<play_tracker::PlayTrackerSnapshot> PrepareCameraSeekState(
    const std::string& checkpoint,
    const play_tracker::PlayTrackerSnapshot& current,
    bool retain_checkpoint_tuning) {
  if (checkpoint.empty() || checkpoint.size() > kMaximumCameraSeekStateBytes)
    return absl::InvalidArgumentError("Camera seek checkpoint is empty or exceeds its metadata bound");
  try {
    auto seed = play_tracker::deserialize_snapshot(checkpoint);
    if (seed.living_boxes.size() != current.living_boxes.size())
      return absl::FailedPreconditionError("Camera seek checkpoint has different camera boxes");
    for (size_t i = 0; i < seed.living_boxes.size(); ++i) {
      const auto& before = seed.living_boxes[i].config;
      const auto& now = current.living_boxes[i].config;
      const auto& a = before.arena_box;
      const auto& b = now.arena_box;
      if (!a || !b || a->left != b->left || a->top != b->top || a->right != b->right || a->bottom != b->bottom ||
          before.name != now.name)
        return absl::FailedPreconditionError("Camera seek checkpoint has different geometry or box roles");
      if (!retain_checkpoint_tuning)
        seed.living_boxes[i].config = now;
    }
    if (!retain_checkpoint_tuning) {
      seed.config = current.config;
      seed.detector = current.detector;
    }
    seed.detector.tracks.clear();
    play_tracker::validate_snapshot(seed);
    return seed;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid camera seek checkpoint: ") + error.what());
  }
}
} // namespace hm::tracking
