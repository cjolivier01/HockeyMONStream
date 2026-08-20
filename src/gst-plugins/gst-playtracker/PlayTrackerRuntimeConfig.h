#pragma once

#include "absl/status/statusor.h"

#include <optional>
#include <string>

inline constexpr int kDefaultZoomInAggressiveness = 25;
inline constexpr int kMinimumZoomInAggressiveness = 0;
inline constexpr int kMaximumZoomInAggressiveness = 100;

// Returns the multiplier applied to the native sticky shrink thresholds.
// The default (25) is exactly 1.0; a smaller multiplier begins zooming sooner.
float DsPlayTrackerZoomInThresholdMultiplier(int aggressiveness);

struct DsPlayTrackerRuntimeTuning {
  std::optional<int> stop_on_dir_change_delay;
  std::optional<bool> cancel_on_opposite;
  std::optional<int> cancel_hysteresis_frames;
  std::optional<int> stop_delay_cooldown_frames;
  std::optional<int> post_nonstop_stop_delay_count;
  std::optional<int> time_to_dest_speed_limit_frames;
  std::optional<int> overshoot_stop_delay_count;
  std::optional<float> overshoot_scale_speed_ratio;
  std::optional<float> max_speed_x;
  std::optional<float> max_speed_y;
  std::optional<float> max_accel_x;
  std::optional<float> max_accel_y;
  std::optional<int> zoom_in_aggressiveness;
  bool apply_to_fast_box{false};
  bool apply_to_follower_box{true};
  bool update_motion_tuning{true};
  std::optional<float> arena_angle_from_vertical;
  std::optional<float> dynamic_acceleration_scaling;
};

absl::StatusOr<DsPlayTrackerRuntimeTuning> DsPlayTrackerLoadRuntimeTuning(const std::string& config_file);
