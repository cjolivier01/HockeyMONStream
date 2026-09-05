#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hm::pipeline_internal {

struct StitchFrameRewindState {
  bool calibration_required{false};
  bool rewind_complete{false};
  bool rewind_pending{false};
};

inline uint64_t stitch_frame_initial_position(
    uint64_t start_time_ns,
    uint64_t stitch_frame_time_ns,
    bool calibration_required,
    bool rewind_complete) {
  return stitch_frame_time_ns > 0 && calibration_required && !rewind_complete ? stitch_frame_time_ns : start_time_ns;
}

inline std::vector<size_t> stitch_frame_rewind_candidates(
    uint64_t stitch_frame_time_ns,
    const std::vector<StitchFrameRewindState>& states) {
  std::vector<size_t> candidates;
  const size_t unfinished_calibration_contexts =
      static_cast<size_t>(std::count_if(states.begin(), states.end(), [](const StitchFrameRewindState& state) {
        return state.calibration_required && !state.rewind_complete;
      }));
  if (stitch_frame_time_ns == 0 && unfinished_calibration_contexts < 2) {
    return candidates;
  }
  for (size_t index = 0; index < states.size(); ++index) {
    const StitchFrameRewindState& state = states[index];
    if (state.calibration_required && !state.rewind_complete && !state.rewind_pending) {
      candidates.push_back(index);
    }
  }
  return candidates;
}

inline bool stitch_frame_rewind_request_is_current(
    long request_stage,
    uint64_t request_generation,
    long current_stage,
    uint64_t current_generation,
    bool main_loop_active) {
  return main_loop_active && request_stage == current_stage && request_generation == current_generation;
}

inline bool stitch_frame_should_account_playback(bool calibration_blocks_playback) {
  return !calibration_blocks_playback;
}

inline bool stitch_output_rotations_are_consistent(const std::vector<double>& rotations) {
  if (rotations.empty() || !std::isfinite(rotations.front()))
    return rotations.empty();
  const double expected = rotations.front() == 0.0 ? 0.0 : rotations.front();
  for (double rotation : rotations) {
    if (!std::isfinite(rotation) || (rotation == 0.0 ? 0.0 : rotation) != expected)
      return false;
  }
  return true;
}

} // namespace hm::pipeline_internal
