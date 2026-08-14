#include "PlaybackProgress.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

bool near(double lhs, double rhs) {
  return std::abs(lhs - rhs) < 0.0001;
}

bool test_rate_recovers_after_pause() {
  hm::PlaybackRateEstimator estimator;
  const auto start = std::chrono::steady_clock::time_point{};
  bool ok = true;
  auto estimate = estimator.sample(0, 100000000000ULL, start, std::chrono::seconds(5));
  ok &= expect(estimate.speed_x == 0.0, "the first rate sample should warm up");

  estimate = estimator.sample(10000000000ULL, 90000000000ULL, start + std::chrono::seconds(5), std::chrono::seconds(5));
  ok &= expect(near(estimate.speed_x, 2.0), "adjacent running samples should report the current rate");
  ok &= expect(estimate.eta_ns == 45000000000ULL, "ETA should use the current adjacent-sample rate");

  estimate =
      estimator.sample(20000000000ULL, 80000000000ULL, start + std::chrono::seconds(70), std::chrono::seconds(5));
  ok &= expect(
      estimate.speed_x == 0.0 && estimate.eta_ns == hm::kUnknownPlaybackTime,
      "the first sample after a long pause should warm up instead of including paused wall time");

  estimate =
      estimator.sample(30000000000ULL, 70000000000ULL, start + std::chrono::seconds(75), std::chrono::seconds(5));
  ok &= expect(near(estimate.speed_x, 2.0), "the next running interval should recover the pre-pause rate");
  ok &= expect(estimate.eta_ns == 35000000000ULL, "post-pause ETA should recover on the next sample");
  return ok;
}

bool test_multi_instance_aggregate_uses_slowest_pipeline() {
  hm::PlaybackProgressMetrics ahead{true, 80000000000ULL, 100000000000ULL, 20000000000ULL, 10000000000ULL, 2.0, 0.8};
  hm::PlaybackProgressMetrics behind{true, 40000000000ULL, 100000000000ULL, 60000000000ULL, 40000000000ULL, 1.5, 0.4};
  hm::PlaybackProgressMetrics aggregate;
  bool ok = expect(
      hm::aggregate_playback_progress({ahead, behind}, &aggregate),
      "valid active pipelines should produce an aggregate");
  ok &= expect(
      aggregate.processed_ns == behind.processed_ns && near(aggregate.fraction, behind.fraction),
      "the aggregate timeline should follow the least-advanced pipeline");
  ok &= expect(near(aggregate.speed_x, 1.5), "the aggregate speed should be the conservative instance rate");
  ok &= expect(aggregate.eta_ns == 40000000000ULL, "the aggregate ETA should be the longest instance ETA");

  behind.speed_x = 0.0;
  behind.eta_ns = hm::kUnknownPlaybackTime;
  ok &= expect(
      hm::aggregate_playback_progress({ahead, behind}, &aggregate) && aggregate.speed_x == 0.0 &&
          aggregate.eta_ns == hm::kUnknownPlaybackTime,
      "an aggregate should remain warming while any active pipeline lacks a rate sample");
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= test_rate_recovers_after_pause();
  ok &= test_multi_instance_aggregate_uses_slowest_pipeline();
  return ok ? 0 : 1;
}
