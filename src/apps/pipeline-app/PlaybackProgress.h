#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <vector>

namespace hm {

constexpr uint64_t kUnknownPlaybackTime = std::numeric_limits<uint64_t>::max();

struct PlaybackProgressMetrics {
  bool valid{false};
  uint64_t processed_ns{kUnknownPlaybackTime};
  uint64_t total_ns{kUnknownPlaybackTime};
  uint64_t remaining_ns{kUnknownPlaybackTime};
  uint64_t eta_ns{kUnknownPlaybackTime};
  double speed_x{0.0};
  double fraction{0.0};
};

class PlaybackRateEstimator {
 public:
  struct Estimate {
    uint64_t eta_ns{kUnknownPlaybackTime};
    double speed_x{0.0};
  };

  Estimate sample(
      uint64_t processed_ns,
      uint64_t remaining_ns,
      std::chrono::steady_clock::time_point now,
      std::chrono::steady_clock::duration expected_sample_interval);
  void reset();

 private:
  bool have_sample_{false};
  uint64_t previous_processed_ns_{0};
  std::chrono::steady_clock::time_point previous_wall_;
};

// Produces one authoritative, conservative value for concurrently running
// pipelines. All active instances must have supplied a valid sample.
bool aggregate_playback_progress(
    const std::vector<PlaybackProgressMetrics>& instances,
    PlaybackProgressMetrics* aggregate);

bool playback_progress_sampling_enabled(bool configured_perf_sampling, bool launched_by_ui);

} // namespace hm
