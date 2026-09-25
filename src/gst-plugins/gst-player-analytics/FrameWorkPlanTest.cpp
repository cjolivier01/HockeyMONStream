#include "hstream/src/gst-plugins/gst-player-analytics/FrameWorkPlan.h"

#include <iostream>

namespace pa = hm::player_analytics;

int main() {
  pa::DueWork all;
  for (size_t feature = 0; feature < 3; ++feature) {
    all.counts[feature] = pa::kMaximumDueRois;
    for (size_t i = 0; i < pa::kMaximumDueRois; ++i)
      all.ids[feature][i] = i + 1;
  }
  unsigned cursor = 0;
  const auto weighted = pa::PlanFrameWork(all, 32, false, &cursor);
  if (weighted.samples != 32 || weighted.counts != std::array<size_t, 3>{{16, 8, 8}})
    return 1;
  std::array<size_t, 3> serviced{};
  cursor = 0;
  for (size_t frame = 0; frame < 40; ++frame) {
    const auto work = pa::PlanFrameWork(all, 1, false, &cursor);
    if (work.samples != 1)
      return 2;
    for (size_t feature = 0; feature < 3; ++feature)
      serviced[feature] += work.counts[feature];
  }
  if (serviced != std::array<size_t, 3>{{20, 10, 10}})
    return 3;
  auto only_pose = all;
  only_pose.counts[1] = only_pose.counts[2] = 0;
  if (pa::PlanFrameWork(only_pose, 32, false, &cursor).counts[0] != 32)
    return 4;
  auto warming = all;
  warming.counts[2] = 0;
  const auto warm = pa::PlanFrameWork(warming, 32, false, &cursor);
  if (warm.samples != 32 || warm.counts[2] || warm.counts[0] < 20)
    return 5;
  cursor = 2;
  const auto pair = pa::PlanFrameWork(all, 2, true, &cursor);
  if (pair.samples != 2 || pair.counts[0] != 1 || pair.counts[1] != 1 || pair.ids[0][0] != pair.ids[1][0])
    return 6;
  pa::DueWork one;
  one.ids[0][0] = one.ids[1][0] = 99;
  one.counts = {{1, 1, 0}};
  cursor = 0;
  const auto reused = pa::PlanFrameWork(one, 2, true, &cursor);
  if (reused.samples != 2 || reused.counts[0] != 1 || reused.counts[1] != 1)
    return 7;
  one.counts[0] = 0;
  if (pa::PlanFrameWork(one, 2, true, &cursor).samples != 0)
    return 8;
  if (pa::PlanFrameWork(all, 0, false, &cursor).samples != 0 ||
      pa::PlanFrameWork(all, 1000, false, &cursor).samples != 32)
    return 9;
  // Guided work near the end of all256 admitted tracks must remain eligible.
  pa::DueWork late;
  late.counts = {{pa::kMaximumTracks, 1, 0}};
  for (size_t i = 0; i < pa::kMaximumTracks; ++i)
    late.ids[0][i] = i + 1;
  late.ids[1][0] = pa::kMaximumTracks;
  cursor = 2;
  const auto last = pa::PlanFrameWork(late, 2, true, &cursor);
  if (last.samples != 2 || last.counts[1] != 1 || last.ids[0][0] != pa::kMaximumTracks)
    return 10;
  std::cout << "Aggregate frame budget, weighted fairness, work lending and same-frame pose pairs passed\n";
}
