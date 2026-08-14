#include "PlaybackProgress.h"

#include <algorithm>

namespace hm {

PlaybackRateEstimator::Estimate PlaybackRateEstimator::sample(
    uint64_t processed_ns,
    uint64_t remaining_ns,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration expected_sample_interval) {
  Estimate estimate;
  if (!have_sample_) {
    have_sample_ = true;
    previous_processed_ns_ = processed_ns;
    previous_wall_ = now;
    return estimate;
  }

  const auto wall_delta = now - previous_wall_;
  const uint64_t processed_delta_ns =
      processed_ns >= previous_processed_ns_ ? processed_ns - previous_processed_ns_ : 0;
  previous_processed_ns_ = processed_ns;
  previous_wall_ = now;

  // A long callback gap normally means the process was SIGSTOP-paused or the
  // pipeline stalled. Do not fold that gap into the displayed rate. Advancing
  // the baseline here makes the next ordinary interval accurate again.
  const auto discontinuity_threshold = expected_sample_interval * 2 + std::chrono::seconds(1);
  if (wall_delta <= std::chrono::steady_clock::duration::zero() || processed_delta_ns == 0 ||
      (expected_sample_interval > std::chrono::steady_clock::duration::zero() &&
       wall_delta > discontinuity_threshold)) {
    return estimate;
  }

  const double wall_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(wall_delta).count();
  estimate.speed_x = (static_cast<double>(processed_delta_ns) / 1000000000.0) / wall_seconds;
  if (remaining_ns != kUnknownPlaybackTime && estimate.speed_x > 0.0) {
    estimate.eta_ns = static_cast<uint64_t>(static_cast<double>(remaining_ns) / estimate.speed_x);
  }
  return estimate;
}

bool aggregate_playback_progress(
    const std::vector<PlaybackProgressMetrics>& instances,
    PlaybackProgressMetrics* aggregate) {
  if (!aggregate || instances.empty() ||
      std::any_of(instances.begin(), instances.end(), [](const auto& metrics) { return !metrics.valid; })) {
    return false;
  }

  const bool any_unknown_total = std::any_of(
      instances.begin(), instances.end(), [](const auto& metrics) { return metrics.total_ns == kUnknownPlaybackTime; });
  const auto slowest =
      std::min_element(instances.begin(), instances.end(), [any_unknown_total](const auto& lhs, const auto& rhs) {
        if (any_unknown_total) {
          return lhs.processed_ns < rhs.processed_ns;
        }
        if (lhs.fraction != rhs.fraction) {
          return lhs.fraction < rhs.fraction;
        }
        return lhs.processed_ns < rhs.processed_ns;
      });
  *aggregate = *slowest;

  aggregate->speed_x = instances.front().speed_x;
  aggregate->eta_ns = instances.front().eta_ns;
  for (const PlaybackProgressMetrics& metrics : instances) {
    if (metrics.speed_x <= 0.0) {
      aggregate->speed_x = 0.0;
    } else if (aggregate->speed_x > 0.0) {
      aggregate->speed_x = std::min(aggregate->speed_x, metrics.speed_x);
    }
    if (metrics.eta_ns == kUnknownPlaybackTime) {
      aggregate->eta_ns = kUnknownPlaybackTime;
    } else if (aggregate->eta_ns != kUnknownPlaybackTime) {
      aggregate->eta_ns = std::max(aggregate->eta_ns, metrics.eta_ns);
    }
  }
  return true;
}

} // namespace hm
