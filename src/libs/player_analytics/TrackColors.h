#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "hstream/src/libs/player_analytics/Types.h"

namespace hm::player_analytics {

struct Color {
  float red;
  float green;
  float blue;
  float alpha;
};

inline constexpr std::array<Color, 32> kTrackPalette{{
    {0.82F, 0.12F, 0.18F, 1}, {0.08F, 0.36F, 0.82F, 1}, {0.08F, 0.58F, 0.24F, 1}, {0.69F, 0.16F, 0.76F, 1},
    {0.90F, 0.40F, 0.04F, 1}, {0.03F, 0.59F, 0.65F, 1}, {0.48F, 0.25F, 0.08F, 1}, {0.78F, 0.17F, 0.47F, 1},
    {0.40F, 0.43F, 0.05F, 1}, {0.32F, 0.16F, 0.69F, 1}, {0.02F, 0.40F, 0.43F, 1}, {0.57F, 0.05F, 0.12F, 1},
    {0.14F, 0.25F, 0.46F, 1}, {0.35F, 0.65F, 0.04F, 1}, {0.89F, 0.25F, 0.28F, 1}, {0.47F, 0.42F, 0.80F, 1},
    {0.02F, 0.64F, 0.47F, 1}, {0.71F, 0.43F, 0.08F, 1}, {0.57F, 0.22F, 0.50F, 1}, {0.13F, 0.52F, 0.81F, 1},
    {0.26F, 0.44F, 0.20F, 1}, {0.64F, 0.34F, 0.27F, 1}, {0.36F, 0.28F, 0.44F, 1}, {0.81F, 0.46F, 0.55F, 1},
    {0.15F, 0.34F, 0.29F, 1}, {0.47F, 0.58F, 0.70F, 1}, {0.61F, 0.54F, 0.18F, 1}, {0.43F, 0.11F, 0.29F, 1},
    {0.28F, 0.55F, 0.54F, 1}, {0.71F, 0.31F, 0.69F, 1}, {0.34F, 0.36F, 0.63F, 1}, {0.39F, 0.33F, 0.25F, 1},
}};

struct ColorLease {
  uint8_t slot{kNoColor};
  // Overflow owners may move when a color becomes available. Established
  // owners retain their slot; uniqueness recovers at <=32 visible tracks.
  bool overflow{false};
};

class TrackColorAllocator {
 public:
  explicit TrackColorAllocator(uint64_t retention_ns = 2 * kSecond) : retention_ns_(retention_ns) {}

  // One allocator per stream/producer. Call once with the complete visible set,
  // before querying any colors. Input is sorted/deduplicated internally; the
  // first 256 IDs are admitted. Invalid IDs are ignored. No heap allocation.
  void Update(uint64_t epoch, uint64_t pts_ns, const uint64_t* ids, size_t count) noexcept;
  void Update(uint64_t epoch, uint64_t pts_ns, const std::vector<uint64_t>& ids) noexcept {
    Update(epoch, pts_ns, ids.data(), ids.size());
  }
  std::optional<ColorLease> Find(uint64_t id) const noexcept;
  void Reset() noexcept;
  size_t size() const noexcept;
  uint64_t excluded_count() const noexcept {
    return excluded_count_;
  }

 private:
  struct Entry {
    uint64_t id{kUntrackedId};
    uint64_t last_seen{0};
    ColorLease lease;
    bool visible{false};
  };
  Entry* Lookup(uint64_t id) noexcept;
  std::optional<uint8_t> TakeFreeColor() noexcept;
  std::array<Entry, kMaximumTracks> entries_{};
  uint64_t retention_ns_;
  uint64_t epoch_{0};
  uint64_t last_pts_{kInvalidTime};
  uint64_t excluded_count_{0};
};

} // namespace hm::player_analytics
