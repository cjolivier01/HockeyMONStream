#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
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
  double output_fps{0.0};
  double output_fps_average{0.0};
};

// Timing from completed output only. Source position queries can report an
// absolute initial seek before the pipeline has processed any video. Keep one
// instance per pipeline and reset it at playback generation boundaries.
class ObservedPlaybackProgress {
 public:
  void observe_pts(uint64_t pts_ns);
  void observe_frame(uint32_t source_id, uint64_t frame_number, uint32_t fps_n, uint32_t fps_d);
  uint64_t processed_ns(uint64_t runtime_offset_ns = 0) const;

 private:
  uint64_t first_pts_ns_{kUnknownPlaybackTime};
  uint64_t elapsed_ns_{kUnknownPlaybackTime};
  std::map<uint32_t, uint64_t> first_frame_by_source_;
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
