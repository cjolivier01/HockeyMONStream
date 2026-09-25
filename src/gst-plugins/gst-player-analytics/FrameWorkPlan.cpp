#include "hstream/src/gst-plugins/gst-player-analytics/FrameWorkPlan.h"

#include <algorithm>

namespace hm::player_analytics {
namespace {
bool Contains(const DueWork& work, size_t feature, uint64_t id) noexcept {
  const size_t count = std::min(work.counts[feature], kMaximumTracks);
  return std::find(work.ids[feature].begin(), work.ids[feature].begin() + count, id) !=
      work.ids[feature].begin() + count;
}
} // namespace

FrameWorkPlan PlanFrameWork(const DueWork& due, size_t budget, bool pose_guided_jersey, unsigned* cursor) noexcept {
  FrameWorkPlan result;
  if (!cursor)
    return result;
  budget = std::min(budget, kMaximumDueRois);
  constexpr std::array<size_t, 4> turns{{0, 0, 1, 2}};
  while (result.samples < budget) {
    bool progressed = false;
    for (size_t turn = 0; turn < turns.size() && !progressed; ++turn) {
      const size_t feature = turns[*cursor % turns.size()];
      *cursor = (*cursor + 1) % turns.size();
      for (size_t i = 0; i < std::min(due.counts[feature], kMaximumTracks); ++i) {
        const uint64_t id = due.ids[feature][i];
        if (id == kUntrackedId || Contains(result, feature, id))
          continue;
        const bool add_pose = feature == 1 && pose_guided_jersey && !Contains(result, 0, id);
        if (add_pose && (!Contains(due, 0, id) || result.samples + 2 > budget))
          continue;
        if (add_pose) {
          result.ids[0][result.counts[0]++] = id;
          ++result.samples;
        }
        result.ids[feature][result.counts[feature]++] = id;
        ++result.samples;
        progressed = true;
        break;
      }
    }
    if (!progressed)
      break;
  }
  return result;
}

} // namespace hm::player_analytics
