#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "hstream/src/libs/player_analytics/Types.h"

namespace hm::player_analytics {

struct DueWork {
  // The scheduler supplies oldest-due order. Action includes only ready, new
  // causal windows; pose-guided jersey is eligible only with same-frame pose.
  std::array<std::array<uint64_t, kMaximumTracks>, 3> ids{};
  std::array<size_t, 3> counts{};
};

struct FrameWorkPlan : DueWork {
  size_t samples{0};
};

// A model sample consumes one aggregate frame-budget unit. Rotating weighted
// turns pose,pose,jersey,action favor evidence collection while preserving fair
// service. Empty/unready queues lend their slots. Pose-guided OCR atomically
// adds a due pose too (two units), or reuses that frame's already planned pose.
// The cursor belongs to the source epoch, not a model or a renderer.
FrameWorkPlan PlanFrameWork(const DueWork& due, size_t budget, bool pose_guided_jersey, unsigned* cursor) noexcept;

} // namespace hm::player_analytics
