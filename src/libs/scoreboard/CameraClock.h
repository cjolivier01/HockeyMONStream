#pragma once

#include <chrono>

namespace hm::scoreboard {
// Decoder/join operation budgets exclude explicitly acknowledged process
// pauses. Media timestamps always retain their independent original clocks.
class CameraClock {
 public:
  using time_point = std::chrono::steady_clock::time_point;
  static time_point now();
  static void SetPaused(bool paused);
};
} // namespace hm::scoreboard
