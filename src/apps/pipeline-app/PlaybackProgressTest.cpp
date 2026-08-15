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

  estimator.reset();
  estimate =
      estimator.sample(31000000000ULL, 69000000000ULL, start + std::chrono::seconds(76), std::chrono::seconds(5));
  ok &= expect(
      estimate.speed_x == 0.0 && estimate.eta_ns == hm::kUnknownPlaybackTime,
      "an explicit resume reset should warm up even after a short pause");
  estimate =
      estimator.sample(41000000000ULL, 59000000000ULL, start + std::chrono::seconds(81), std::chrono::seconds(5));
  ok &= expect(near(estimate.speed_x, 2.0), "an explicit reset should recover from a short pause in one interval");
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

  hm::PlaybackProgressMetrics unknown_total{
      true, 90000000000ULL, hm::kUnknownPlaybackTime, hm::kUnknownPlaybackTime, hm::kUnknownPlaybackTime, 1.0, 0.0};
  ok &= expect(
      hm::aggregate_playback_progress({behind, unknown_total}, &aggregate) &&
          aggregate.total_ns == hm::kUnknownPlaybackTime && aggregate.remaining_ns == hm::kUnknownPlaybackTime &&
          aggregate.fraction == 0.0,
      "one unknown instance duration should keep the aggregate timeline indeterminate");
  return ok;
}

bool test_multi_instance_short_pause_requires_fresh_samples() {
  hm::PlaybackRateEstimator left;
  hm::PlaybackRateEstimator right;
  const auto start = std::chrono::steady_clock::time_point{};
  left.sample(0, 100000000000ULL, start, std::chrono::seconds(5));
  right.sample(0, 100000000000ULL, start, std::chrono::seconds(5));
  left.sample(10000000000ULL, 90000000000ULL, start + std::chrono::seconds(5), std::chrono::seconds(5));
  right.sample(10000000000ULL, 90000000000ULL, start + std::chrono::seconds(5), std::chrono::seconds(5));

  left.reset();
  right.reset();
  auto left_rate =
      left.sample(12000000000ULL, 88000000000ULL, start + std::chrono::seconds(7), std::chrono::seconds(5));
  auto right_rate =
      right.sample(12000000000ULL, 88000000000ULL, start + std::chrono::seconds(7), std::chrono::seconds(5));
  hm::PlaybackProgressMetrics left_progress{
      true, 12000000000ULL, 100000000000ULL, 88000000000ULL, left_rate.eta_ns, left_rate.speed_x, 0.12};
  hm::PlaybackProgressMetrics right_progress{
      true, 12000000000ULL, 100000000000ULL, 88000000000ULL, right_rate.eta_ns, right_rate.speed_x, 0.12};
  hm::PlaybackProgressMetrics aggregate;
  bool ok = expect(
      hm::aggregate_playback_progress({left_progress, right_progress}, &aggregate) && aggregate.speed_x == 0.0,
      "both instances should warm up after an explicit short-pause reset");

  left_rate = left.sample(22000000000ULL, 78000000000ULL, start + std::chrono::seconds(12), std::chrono::seconds(5));
  left_progress.speed_x = left_rate.speed_x;
  left_progress.eta_ns = left_rate.eta_ns;
  ok &= expect(
      hm::aggregate_playback_progress({left_progress, right_progress}, &aggregate) && aggregate.speed_x == 0.0,
      "the aggregate should remain warming until every instance has a fresh adjacent sample");

  right_rate = right.sample(22000000000ULL, 78000000000ULL, start + std::chrono::seconds(12), std::chrono::seconds(5));
  right_progress.speed_x = right_rate.speed_x;
  right_progress.eta_ns = right_rate.eta_ns;
  ok &= expect(
      hm::aggregate_playback_progress({left_progress, right_progress}, &aggregate) && near(aggregate.speed_x, 2.0),
      "the aggregate should recover only after every instance has a fresh adjacent sample");
  return ok;
}

bool test_ui_launch_enables_progress_sampling() {
  bool ok = expect(
      hm::playback_progress_sampling_enabled(false, true),
      "a UI launch should sample progress even when ordinary FPS reporting is disabled");
  ok &= expect(
      hm::playback_progress_sampling_enabled(true, false),
      "configured command-line performance sampling should remain enabled");
  ok &= expect(
      !hm::playback_progress_sampling_enabled(false, false),
      "non-UI runs should continue respecting disabled performance sampling");
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= test_rate_recovers_after_pause();
  ok &= test_multi_instance_aggregate_uses_slowest_pipeline();
  ok &= test_multi_instance_short_pause_requires_fresh_samples();
  ok &= test_ui_launch_enables_progress_sampling();
  return ok ? 0 : 1;
}
