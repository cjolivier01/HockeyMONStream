#pragma once

#include <opencv2/core.hpp>
#include <array>
#include <cstdint>

namespace hm::draw_display {

// Display-only filtering in rink metres, driven by source time. Four-way
// fixed-size buckets bound each lookup and require no heap allocations. A
// collision evicts the oldest history; it cannot blend unrelated players.
class RinkTrackSmoothing {
 public:
  void Reset();
  cv::Point2d Update(uint64_t track, uint64_t pts_ns, uint32_t color, cv::Point2d measured);

 private:
  struct State {
    uint64_t track{0}, pts_ns{0};
    uint32_t color{0};
    unsigned samples{0};
    cv::Point2d previous, older, filtered, velocity;
  };
  std::array<std::array<State, 4>, 128> histories_{};
};
} // namespace hm::draw_display
