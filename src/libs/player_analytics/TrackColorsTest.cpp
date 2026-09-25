#include "hstream/src/libs/player_analytics/TrackColors.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <vector>

namespace pa = hm::player_analytics;

namespace {
bool Unique(const pa::TrackColorAllocator& colors, const std::vector<uint64_t>& ids) {
  std::set<uint8_t> slots;
  for (uint64_t id : ids) {
    const auto lease = colors.Find(id);
    if (!lease || lease->overflow || !slots.insert(lease->slot).second)
      return false;
  }
  return true;
}
bool Check(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}
} // namespace

int main() {
  pa::TrackColorAllocator colors;
  std::vector<uint64_t> ids;
  for (uint64_t i = 0; i < 32; ++i)
    ids.push_back((1ULL << 40) + i * 32); // Every ID would collide under modulo.
  colors.Update(1, 0, ids);
  bool ok = Check(Unique(colors, ids), "32 visible full-width IDs must have distinct colors");
  const auto first_slot = colors.Find(ids[0])->slot;
  std::reverse(ids.begin(), ids.end());
  colors.Update(1, 1, ids);
  ok &= Check(Unique(colors, ids) && colors.Find(ids.back())->slot == first_slot, "input order changed leases");

  ids.push_back(9999);
  colors.Update(1, 2, ids);
  ok &= Check(colors.Find(9999) && colors.Find(9999)->overflow, "33rd visible track should share temporarily");
  const uint8_t shared_slot = colors.Find(9999)->slot;
  const auto removed = std::find_if(
      ids.begin(), ids.end(), [&](uint64_t id) { return id != 9999 && colors.Find(id)->slot != shared_slot; });
  const uint64_t removed_id = *removed;
  ids.erase(removed);
  colors.Update(1, 3, ids);
  ok &= Check(Unique(colors, ids), "33 to 32 must recover uniqueness even when another color was released");
  ok &= Check(!colors.Find(removed_id), "inactive lease must be reclaimed before sharing");

  ids = {1000, 1001};
  colors.Update(1, 4, ids);
  ok &= Check(Unique(colors, ids), "missing-track reservations must not cause visible collisions");
  const auto retained = colors.Find(1000)->slot;
  colors.Update(1, 5, std::vector<uint64_t>{1001});
  colors.Update(1, 6, ids);
  ok &= Check(colors.Find(1000)->slot == retained, "short absence lost an available lease");
  colors.Update(1, 3 * pa::kSecond, std::vector<uint64_t>{1001});
  ok &= Check(!colors.Find(1000), "source-time expiry retained a missing track");
  colors.Update(2, 4 * pa::kSecond, std::vector<uint64_t>{77, 77, pa::kUntrackedId});
  ok &= Check(colors.size() == 1 && colors.Find(77) && !colors.Find(1001), "epoch/duplicate/sentinel handling");
  colors.Update(2, 1, std::vector<uint64_t>{88});
  ok &= Check(colors.size() == 1 && !colors.Find(77), "backward source time must reset leases");
  colors.Update(2, pa::kInvalidTime, ids);
  ok &= Check(colors.size() == 0, "invalid clock must not retain identities");
  ids.clear();
  for (uint64_t i = 0; i < 300; ++i)
    ids.push_back(i);
  colors.Update(3, 0, ids);
  ok &= Check(colors.size() == pa::kMaximumTracks && colors.excluded_count() == 44, "hard bound ignored");
  return ok ? 0 : 1;
}
