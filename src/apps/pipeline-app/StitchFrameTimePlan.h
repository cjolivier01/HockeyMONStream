#pragma once

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
  if (stitch_frame_time_ns == 0) {
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

} // namespace hm::pipeline_internal
