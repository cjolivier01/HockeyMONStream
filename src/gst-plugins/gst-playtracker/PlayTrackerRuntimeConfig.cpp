#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerRuntimeConfig.h"

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

float DsPlayTrackerZoomInThresholdMultiplier(int aggressiveness) {
  aggressiveness = std::clamp(aggressiveness, kMinimumZoomInAggressiveness, kMaximumZoomInAggressiveness);
  if (aggressiveness <= kDefaultZoomInAggressiveness) {
    return 2.0f - static_cast<float>(aggressiveness) / static_cast<float>(kDefaultZoomInAggressiveness);
  }
  constexpr float kMinimumMultiplier = 0.1f;
  const float normalized = static_cast<float>(aggressiveness - kDefaultZoomInAggressiveness) /
      static_cast<float>(kMaximumZoomInAggressiveness - kDefaultZoomInAggressiveness);
  return 1.0f - normalized * (1.0f - kMinimumMultiplier);
}

absl::StatusOr<DsPlayTrackerRuntimeTuning> DsPlayTrackerLoadRuntimeTuning(const std::string& config_file) {
  if (config_file.empty())
    return absl::InvalidArgumentError("playtracker runtime config-file is empty");
  try {
    const YAML::Node document = YAML::LoadFile(config_file);
    const YAML::Node play_tracker = document["play-tracker"];
    if (!play_tracker || !play_tracker.IsMap())
      return absl::InvalidArgumentError("playtracker runtime config must contain a play-tracker map");
    const YAML::Node runtime = play_tracker["hstream-runtime-tuning"];
    if (!runtime || !runtime.IsMap())
      return absl::InvalidArgumentError("playtracker runtime config must contain a hstream-runtime-tuning map");
    auto read_int = [](const YAML::Node& node, const char* key) -> std::optional<int> {
      if (!node[key])
        return std::nullopt;
      if (!node[key].IsScalar())
        throw std::invalid_argument(absl::StrCat(key, " must be a scalar"));
      return node[key].as<int>();
    };
    auto read_float = [](const YAML::Node& node, const char* key) -> std::optional<float> {
      if (!node[key])
        return std::nullopt;
      if (!node[key].IsScalar())
        throw std::invalid_argument(absl::StrCat(key, " must be a scalar"));
      const float value = node[key].as<float>();
      if (!std::isfinite(value))
        throw std::invalid_argument(absl::StrCat(key, " must be finite"));
      return value;
    };
    auto read_bool = [](const YAML::Node& node, const char* key, bool fallback) {
      if (!node[key])
        return fallback;
      if (!node[key].IsScalar())
        throw std::invalid_argument(absl::StrCat(key, " must be a scalar"));
      return node[key].as<bool>();
    };
    const bool apply_to_fast = read_bool(play_tracker, "hstream-apply-to-fast-box", false);
    const bool apply_to_follower = read_bool(play_tracker, "hstream-apply-to-follower-box", true);
    const YAML::Node live_boxes = play_tracker["live-boxes"];
    const size_t live_box_count = live_boxes && live_boxes.IsSequence() ? live_boxes.size() : 0;
    const std::optional<int> zoom_in_aggressiveness = read_int(runtime, "zoom-in-aggressiveness");
    if (zoom_in_aggressiveness.has_value() &&
        (*zoom_in_aggressiveness < kMinimumZoomInAggressiveness ||
         *zoom_in_aggressiveness > kMaximumZoomInAggressiveness)) {
      return absl::InvalidArgumentError("zoom-in-aggressiveness must be from 0 through 100");
    }
    if (apply_to_fast && live_box_count < 1)
      return absl::FailedPreconditionError("playtracker runtime tuning requires a fast live box");
    if (apply_to_follower && live_box_count < 1)
      return absl::FailedPreconditionError("playtracker runtime tuning requires a follower live box");
    return DsPlayTrackerRuntimeTuning{
        .stop_on_dir_change_delay = read_int(runtime, "stop-translation-on-dir-change-delay"),
        .cancel_on_opposite = runtime["cancel-stop-on-opposite-dir"]
            ? std::optional<bool>(read_bool(runtime, "cancel-stop-on-opposite-dir", false))
            : std::nullopt,
        .cancel_hysteresis_frames = read_int(runtime, "cancel-stop-hysteresis-frames"),
        .stop_delay_cooldown_frames = read_int(runtime, "stop-delay-cooldown-frames"),
        .post_nonstop_stop_delay_count = read_int(runtime, "post-nonstop-stop-delay-count"),
        .time_to_dest_speed_limit_frames = read_int(runtime, "time-to-dest-speed-limit-frames"),
        .overshoot_stop_delay_count = read_int(runtime, "overshoot-stop-delay-count"),
        .overshoot_scale_speed_ratio = read_float(runtime, "overshoot-scale-speed-ratio"),
        .max_speed_x = read_float(runtime, "max-speed-x"),
        .max_speed_y = read_float(runtime, "max-speed-y"),
        .max_accel_x = read_float(runtime, "max-accel-x"),
        .max_accel_y = read_float(runtime, "max-accel-y"),
        .zoom_in_aggressiveness = zoom_in_aggressiveness,
        .apply_to_fast_box = apply_to_fast,
        .apply_to_follower_box = apply_to_follower,
        .arena_angle_from_vertical = read_float(runtime, "arena-angle-from-vertical"),
        .dynamic_acceleration_scaling = read_float(runtime, "dynamic-acceleration-scaling"),
    };
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(absl::StrCat("invalid playtracker runtime config: ", error.what()));
  }
}
