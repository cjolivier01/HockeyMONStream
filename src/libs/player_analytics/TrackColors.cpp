#include "hstream/src/libs/player_analytics/TrackColors.h"

#include <algorithm>

namespace hm::player_analytics {

void TrackColorAllocator::Reset() noexcept {
  entries_ = {};
  epoch_ = 0;
  last_pts_ = kInvalidTime;
  excluded_count_ = 0;
}

size_t TrackColorAllocator::size() const noexcept {
  return std::count_if(entries_.begin(), entries_.end(), [](const Entry& entry) { return entry.id != kUntrackedId; });
}

TrackColorAllocator::Entry* TrackColorAllocator::Lookup(uint64_t id) noexcept {
  if (id == kUntrackedId)
    return nullptr;
  for (auto& entry : entries_)
    if (entry.id == id)
      return &entry;
  return nullptr;
}

std::optional<ColorLease> TrackColorAllocator::Find(uint64_t id) const noexcept {
  if (id == kUntrackedId)
    return std::nullopt;
  for (const auto& entry : entries_)
    if (entry.id == id && entry.lease.slot < kTrackPalette.size())
      return entry.lease;
  return std::nullopt;
}

std::optional<uint8_t> TrackColorAllocator::TakeFreeColor() noexcept {
  // Inactive leases are expendable only when there is no unused slot. If an
  // inactive shared lease does not free its slot, continue to the next oldest.
  for (size_t attempt = 0; attempt <= entries_.size(); ++attempt) {
    std::array<bool, kTrackPalette.size()> used{};
    Entry* oldest = nullptr;
    for (auto& entry : entries_) {
      if (entry.id == kUntrackedId)
        continue;
      if (entry.lease.slot < used.size())
        used[entry.lease.slot] = true;
      if (!entry.visible &&
          (!oldest || entry.last_seen < oldest->last_seen ||
           (entry.last_seen == oldest->last_seen && entry.id < oldest->id)))
        oldest = &entry;
    }
    for (size_t slot = 0; slot < used.size(); ++slot)
      if (!used[slot])
        return static_cast<uint8_t>(slot);
    if (!oldest)
      return std::nullopt;
    *oldest = {};
  }
  return std::nullopt;
}

void TrackColorAllocator::Update(uint64_t epoch, uint64_t pts_ns, const uint64_t* ids, size_t count) noexcept {
  if (pts_ns == kInvalidTime || (!ids && count != 0)) {
    Reset();
    return;
  }
  if (last_pts_ != kInvalidTime && (epoch != epoch_ || pts_ns < last_pts_))
    Reset();
  epoch_ = epoch;
  last_pts_ = pts_ns;

  std::array<uint64_t, kMaximumTracks> sorted{};
  const size_t inspected = std::min(count, sorted.size());
  excluded_count_ += count - inspected;
  size_t valid = 0;
  for (size_t i = 0; i < inspected; ++i)
    if (ids[i] != kUntrackedId)
      sorted[valid++] = ids[i];
  std::sort(sorted.begin(), sorted.begin() + valid);
  valid = std::unique(sorted.begin(), sorted.begin() + valid) - sorted.begin();

  for (auto& entry : entries_) {
    if (entry.id == kUntrackedId)
      continue;
    entry.visible = std::binary_search(sorted.begin(), sorted.begin() + valid, entry.id);
    if (entry.visible)
      entry.last_seen = pts_ns;
    else if (pts_ns - entry.last_seen > retention_ns_)
      entry = {};
  }

  // Recover temporary shared colors first. Excluding the overflow owner's old
  // claim also promotes it if the original established owner has disappeared.
  for (size_t i = 0; i < valid; ++i) {
    Entry* entry = Lookup(sorted[i]);
    if (!entry || !entry->lease.overflow)
      continue;
    const uint8_t old_slot = entry->lease.slot;
    entry->lease.slot = kNoColor;
    const auto slot = TakeFreeColor();
    entry->lease = slot ? ColorLease{*slot, false} : ColorLease{old_slot, true};
  }

  for (size_t i = 0; i < valid; ++i) {
    if (Lookup(sorted[i]))
      continue;
    const auto free_color = TakeFreeColor();
    Entry* available = nullptr;
    for (auto& entry : entries_)
      if (entry.id == kUntrackedId) {
        available = &entry;
        break;
      }
    if (!available) {
      ++excluded_count_;
      continue;
    }
    ColorLease lease;
    if (free_color) {
      lease = {*free_color, false};
    } else {
      std::array<size_t, kTrackPalette.size()> usage{};
      for (const auto& entry : entries_)
        if (entry.id != kUntrackedId && entry.lease.slot < usage.size())
          ++usage[entry.lease.slot];
      lease = {static_cast<uint8_t>(std::min_element(usage.begin(), usage.end()) - usage.begin()), true};
    }
    *available = Entry{sorted[i], pts_ns, lease, true};
  }
}

} // namespace hm::player_analytics
