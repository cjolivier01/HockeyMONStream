#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerCtx.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "hockeymom/csrc/play_tracker/PlayTracker.h"
#include "hockeymom/csrc/play_tracker/ResizingBox.h"
#include "hockeymom/csrc/play_tracker/TranslatingBox.h"
#include "hstream/src/gst-plugins/gst-fieldmask/fieldmask_payload.h"
#include "hstream/src/libs/common/ConfigYaml.h"
#include "hstream/src/libs/common/PlotContext.h"

#include <nvdsmeta.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <set>

#include <stdio.h>
#include <stdlib.h>

namespace gst_hm_playtracker {

using namespace hm;
using namespace hm::play_tracker;

bool validate_numeric_yaml_fields(const YAML::Node& node, std::string* error) {
  static const std::set<std::string> kNumericKeys = {
      "arena-angle-from-vertical",
      "dynamic-acceleration-scaling",
      "fps-speed-scale",
      "frame-step",
      "group-ratio-threshold",
      "group-velocity-speed-ratio",
      "max-accel-h",
      "max-accel-w",
      "max-accel-x",
      "max-accel-y",
      "max-lost-track-age",
      "max-positions",
      "max-speed-h",
      "max-speed-ratio-x",
      "max-speed-ratio-y",
      "max-speed-w",
      "max-speed-x",
      "max-speed-y",
      "max-accel-ratio-x",
      "max-accel-ratio-y",
      "max-velocity-positions",
      "max-width",
      "max-height",
      "min-considered-group-velocity",
      "min-width",
      "min-height",
      "nonstop-delay-count",
      "overshoot-stop-delay-count",
      "overshoot-scale-speed-ratio",
      "scale-dest-height",
      "scale-dest-width",
      "scale-speed-constraints",
      "resizing-stop-cancel-hysteresis-frames",
      "resizing-stop-delay-cooldown-frames",
      "resizing-stop-on-dir-change-delay",
      "resizing-time-to-dest-speed-limit-frames",
      "resizing-time-to-dest-stop-speed-threshold",
      "size-ratio-thresh-grow-dh",
      "size-ratio-thresh-grow-dw",
      "size-ratio-thresh-shrink-dh",
      "size-ratio-thresh-shrink-dw",
      "sticky-size-ratio-to-frame-width",
      "sticky-translation-gaussian-mult",
      "stop-delay-cooldown-frames",
      "stop-translation-on-dir-change-delay",
      "cancel-stop-hysteresis-frames",
      "post-nonstop-stop-delay-count",
      "time-to-dest-speed-limit-frames",
      "time-to-dest-stop-speed-threshold",
      "unsticky-translation-size-ratio",
      "follower-box-min-height-ratio",
  };
  if (!node) {
    return true;
  }
  if (node.IsSequence()) {
    for (const auto& item : node) {
      if (!validate_numeric_yaml_fields(item, error)) {
        return false;
      }
    }
    return true;
  }
  if (!node.IsMap()) {
    return true;
  }
  for (const auto& entry : node) {
    if (!entry.first.IsScalar()) {
      continue;
    }
    std::string key = entry.first.as<std::string>();
    std::replace(key.begin(), key.end(), '_', '-');
    const YAML::Node value = entry.second;
    if (kNumericKeys.count(key) && value && value.IsScalar()) {
      try {
        const double parsed = value.as<double>();
        if (!std::isfinite(parsed)) {
          if (error) {
            *error = absl::StrCat("invalid non-finite numeric value for ", key);
          }
          return false;
        }
      } catch (const std::exception& exc) {
        if (error) {
          *error = absl::StrCat("invalid numeric value for ", key, ": ", exc.what());
        }
        return false;
      }
    }
    if (!validate_numeric_yaml_fields(value, error)) {
      return false;
    }
  }
  return true;
}

PlayDetectorConfig create_play_detector_config(
    PlayDetectorConfig& config,
    const YAML::Node& yaml,
    hm::utils::ConfigLocator& locator) {
  SET_LOCATOR(locator, config, max_positions);
  SET_LOCATOR(locator, config, max_velocity_positions);
  SET_LOCATOR(locator, config, frame_step);
  SET_LOCATOR(locator, config, fps_speed_scale);
  SET_LOCATOR(locator, config, min_considered_group_velocity);
  SET_LOCATOR(locator, config, group_ratio_threshold);
  SET_LOCATOR(locator, config, group_velocity_speed_ratio);
  SET_LOCATOR(locator, config, scale_speed_constraints);
  SET_LOCATOR(locator, config, nonstop_delay_count);
  SET_LOCATOR(locator, config, overshoot_scale_speed_ratio);
  SET_LOCATOR(locator, config, overshoot_stop_delay_count);
  return config;
}

ResizingConfig create_resizing_config(
    ResizingConfig& config,
    const YAML::Node& yaml,
    hm::utils::ConfigLocator& locator) {
  SET_LOCATOR(locator, config, resizing_enabled);
  SET_LOCATOR(locator, config, max_speed_w);
  SET_LOCATOR(locator, config, max_speed_h);
  SET_LOCATOR(locator, config, max_accel_w);
  SET_LOCATOR(locator, config, max_accel_h);
  SET_LOCATOR(locator, config, min_width);
  SET_LOCATOR(locator, config, min_height);
  SET_LOCATOR(locator, config, max_width);
  SET_LOCATOR(locator, config, max_height);
  SET_LOCATOR(locator, config, stop_resizing_on_dir_change);
  SET_LOCATOR(locator, config, resizing_stop_on_dir_change_delay);
  SET_LOCATOR(locator, config, resizing_cancel_stop_on_opposite_dir);
  SET_LOCATOR(locator, config, resizing_stop_cancel_hysteresis_frames);
  SET_LOCATOR(locator, config, resizing_stop_delay_cooldown_frames);
  SET_LOCATOR(locator, config, resizing_time_to_dest_speed_limit_frames);
  SET_LOCATOR(locator, config, resizing_time_to_dest_stop_speed_threshold);
  SET_LOCATOR(locator, config, sticky_sizing);
  SET_LOCATOR(locator, config, size_ratio_thresh_grow_dw);
  SET_LOCATOR(locator, config, size_ratio_thresh_grow_dh);
  SET_LOCATOR(locator, config, size_ratio_thresh_shrink_dw);
  SET_LOCATOR(locator, config, size_ratio_thresh_shrink_dh);
  return config;
}
TranslatingBoxConfig create_translating_box_config(
    const BBox& arena_box,
    TranslatingBoxConfig& config,
    const YAML::Node& yaml,
    hm::utils::ConfigLocator& locator) {
  SET_LOCATOR(locator, config, translation_enabled);
  SET_LOCATOR(locator, config, max_speed_x);
  SET_LOCATOR(locator, config, max_speed_y);
  SET_LOCATOR(locator, config, max_accel_x);
  SET_LOCATOR(locator, config, max_accel_y);
  SET_LOCATOR(locator, config, stop_translation_on_dir_change);
  SET_LOCATOR(locator, config, stop_translation_on_dir_change_delay);
  SET_LOCATOR(locator, config, cancel_stop_on_opposite_dir);
  SET_LOCATOR(locator, config, post_nonstop_stop_delay_count);
  SET_LOCATOR(locator, config, cancel_stop_hysteresis_frames);
  SET_LOCATOR(locator, config, stop_delay_cooldown_frames);
  SET_LOCATOR(locator, config, time_to_dest_speed_limit_frames);
  SET_LOCATOR(locator, config, time_to_dest_stop_speed_threshold);
  SET_LOCATOR(locator, config, sticky_translation);
  SET_LOCATOR(locator, config, sticky_size_ratio_to_frame_width);
  SET_LOCATOR(locator, config, sticky_translation_gaussian_mult);
  SET_LOCATOR(locator, config, unsticky_translation_size_ratio);
  SET_LOCATOR(locator, config, dynamic_acceleration_scaling);
  SET_LOCATOR(locator, config, arena_angle_from_vertical);
  config.arena_box = arena_box;
  return config;
}

LivingBoxConfig create_living_box_config(
    LivingBoxConfig& config,
    const YAML::Node& yaml,
    hm::utils::ConfigLocator& locator,
    std::optional<FloatValue> fixed_aspect_ratio = std::nullopt) {
  SET_LOCATOR(locator, config, scale_dest_width);
  SET_LOCATOR(locator, config, scale_dest_height);
  SET_LOCATOR(locator, config, clamp_scaled_input_box);
  config.fixed_aspect_ratio = fixed_aspect_ratio;
  return config;
}

void apply_all_living_box_config(
    const BBox& arena_box,
    AllLivingBoxConfig& config,
    const YAML::Node& yaml,
    std::optional<FloatValue> fixed_aspect_ratio = std::nullopt) {
  hm::utils::ConfigLocator locator;
  SET_LOCATOR(locator, config, name);
  *((ResizingConfig*)&config) = create_resizing_config(config, yaml, locator);
  *((TranslatingBoxConfig*)&config) = create_translating_box_config(arena_box, config, yaml, locator);
  *((LivingBoxConfig*)&config) = create_living_box_config(config, yaml, locator, fixed_aspect_ratio);
  set_config_from_yaml(yaml, locator);
}

AllLivingBoxConfig create_all_living_box_config(
    const BBox& arena_box,
    const YAML::Node& yaml,
    std::optional<FloatValue> fixed_aspect_ratio = std::nullopt) {
  AllLivingBoxConfig config;
  apply_all_living_box_config(arena_box, config, yaml, fixed_aspect_ratio);
  return config;
}

const std::unordered_map<std::string, float> CAMERA_TYPE_MAX_SPEEDS = {
    {"GoPro", 200.0},
    {"Zhiwei", 200.0},
    {"LiveBarn", 300.0},
};

void adjust_config(const BBox& arena_box, PlayTrackerConfig& pt_config, const std::string& camera = "GoPro") {
  const float max_camera_speed = CAMERA_TYPE_MAX_SPEEDS.at(camera);
  const float scale = pt_config.play_detector.fps_speed_scale;
  const float camera_box_max_speed_x = std::max(arena_box.width() / max_camera_speed, 12.0f);
  const float camera_box_max_speed_y = std::max(arena_box.height() / max_camera_speed, 12.0f);
  const float camera_box_max_accel_x = 1.0 / scale;
  const float camera_box_max_accel_y = 1.0 / scale;
  // Do the "fast" boxes
  for (int i = 0; i < int(pt_config.living_boxes.size()) - 1; ++i) {
    AllLivingBoxConfig& bcfg = pt_config.living_boxes.at(i);
    // Translation
    bcfg.max_speed_x = camera_box_max_speed_x * 1.5 / scale;
    bcfg.max_speed_y = camera_box_max_speed_y * 1.5 / scale;
    bcfg.max_accel_x = camera_box_max_accel_x * 1.1 / scale;
    bcfg.max_accel_y = camera_box_max_accel_y * 1.1 / scale;
    // Resizing
    bcfg.max_speed_w = camera_box_max_speed_x * 1.5 / scale / 1.8;
    bcfg.max_speed_h = camera_box_max_speed_y * 1.5 / scale / 1.8;
    bcfg.max_accel_w = camera_box_max_accel_x * 1.1 / scale;
    bcfg.max_accel_h = camera_box_max_accel_y * 1.1 / scale;
    bcfg.max_width = arena_box.width();
    bcfg.max_height = arena_box.height();
    bcfg.min_height = 10;
    ((ResizingConfig*)&bcfg)->stop_resizing_on_dir_change = false;
    ((TranslatingBoxConfig*)&bcfg)->stop_translation_on_dir_change = false;
    bcfg.sticky_sizing = false;
    bcfg.sticky_translation = false;
    bcfg.arena_box = arena_box;
  }
  // "Do the final box
  if (!pt_config.living_boxes.empty()) {
    AllLivingBoxConfig& bcfg = pt_config.living_boxes.back();
    // Translation
    bcfg.max_speed_x = camera_box_max_speed_x / scale;
    bcfg.max_speed_y = camera_box_max_speed_y / scale;
    bcfg.max_accel_x = camera_box_max_accel_x / scale;
    bcfg.max_accel_y = camera_box_max_accel_y / scale;
    // Resizing
    bcfg.max_speed_w = camera_box_max_speed_x / scale / 1.8;
    bcfg.max_speed_h = camera_box_max_speed_y / scale / 1.8;
    bcfg.max_accel_w = camera_box_max_accel_x / scale;
    bcfg.max_accel_h = camera_box_max_accel_y / scale;
    bcfg.max_width = arena_box.width();
    bcfg.max_height = arena_box.height();
    bcfg.min_height = arena_box.height() / 5;
    ((ResizingConfig*)&bcfg)->stop_resizing_on_dir_change = true;
    ((TranslatingBoxConfig*)&bcfg)->stop_translation_on_dir_change = true;
    bcfg.sticky_sizing = true;
    bcfg.sticky_translation = true;
    bcfg.arena_box = arena_box;

    bcfg.dynamic_acceleration_scaling = true; // EXPERIMENTA
    // bcfg.dynamic_acceleration_scaling = false;
  }
}

PlayTrackerConfig create_play_tracker_config(const BBox& arena_box, const YAML::Node& yaml) {
  PlayTrackerConfig config;
  hm::utils::ConfigLocator locator{
      .ignored{
          "camera-name",
          "follower-box-min-height-ratio",
          "hstream-apply-to-fast-box",
          "hstream-apply-to-follower-box",
          "hstream-runtime-tuning",
          "live-boxes",
          "max-accel-ratio-x",
          "max-accel-ratio-y",
          "max-speed-ratio-x",
          "max-speed-ratio-y",
      },
  };
  std::vector<YAML::Node> live_box_yamls;

  if (yaml["live-boxes"]) {
    YAML::Node live_boxes = yaml["live-boxes"];
    // Iterate over the list
    for (const auto& box_yaml : live_boxes) {
      live_box_yamls.emplace_back(box_yaml);
      config.living_boxes.emplace_back(create_all_living_box_config(arena_box, box_yaml));
    }
    if (!config.living_boxes.empty()) {
      // Last one gets fixed aspect ratio
      config.living_boxes.back().fixed_aspect_ratio = 16.0 / 9.0;
    }
  }
  config.play_detector = create_play_detector_config(config.play_detector, yaml, locator);

  config.ignore_outlier_players = true; // EXPERIMENTAL
  config.ignore_left_and_right_extremes = false; // EXPERIMENTAL

  const std::string camera_name = yaml["camera-name"] ? yaml["camera-name"].as<std::string>() : "GoPro";
  adjust_config(arena_box, config, camera_name);
  const float max_speed_ratio_x = yaml["max-speed-ratio-x"] ? yaml["max-speed-ratio-x"].as<float>() : 1.0f;
  const float max_speed_ratio_y = yaml["max-speed-ratio-y"] ? yaml["max-speed-ratio-y"].as<float>() : 1.0f;
  const float max_accel_ratio_x = yaml["max-accel-ratio-x"] ? yaml["max-accel-ratio-x"].as<float>() : 1.0f;
  const float max_accel_ratio_y = yaml["max-accel-ratio-y"] ? yaml["max-accel-ratio-y"].as<float>() : 1.0f;
  for (AllLivingBoxConfig& box : config.living_boxes) {
    box.max_speed_x *= max_speed_ratio_x;
    box.max_speed_y *= max_speed_ratio_y;
    box.max_accel_x *= max_accel_ratio_x;
    box.max_accel_y *= max_accel_ratio_y;
    box.max_speed_w *= max_speed_ratio_x;
    box.max_speed_h *= max_speed_ratio_y;
    box.max_accel_w *= max_accel_ratio_x;
    box.max_accel_h *= max_accel_ratio_y;
  }
  if (!config.living_boxes.empty() && yaml["follower-box-min-height-ratio"]) {
    config.living_boxes.back().min_height = arena_box.height() * yaml["follower-box-min-height-ratio"].as<float>();
  }
  for (size_t i = 0; i < live_box_yamls.size() && i < config.living_boxes.size(); ++i) {
    const std::optional<FloatValue> fixed_aspect_ratio =
        i + 1 == config.living_boxes.size() ? std::optional<FloatValue>(16.0 / 9.0) : std::nullopt;
    apply_all_living_box_config(arena_box, config.living_boxes[i], live_box_yamls[i], fixed_aspect_ratio);
  }
  SET_LOCATOR(locator, config, no_wide_start);
  SET_LOCATOR(locator, config, max_lost_track_age);
  SET_LOCATOR(locator, config, ignore_largest_bbox);
  set_config_from_yaml(yaml, locator);

  return config;
}

absl::Status validate_runtime_tuning_target(
    const DsPlayTrackerCtx::PlayTracker& tracker_context,
    const DsPlayTrackerRuntimeTuning& tuning) {
  const size_t box_count = tracker_context.play_tracker_config.living_boxes.size();
  if (tuning.apply_to_fast_box && box_count < 1) {
    return absl::FailedPreconditionError("playtracker runtime tuning requires a fast live box");
  }
  if (tuning.apply_to_follower_box && box_count < 2) {
    return absl::FailedPreconditionError("playtracker runtime tuning requires a follower live box");
  }
  if (tracker_context.base_play_tracker_config.living_boxes.size() != box_count) {
    return absl::FailedPreconditionError("playtracker runtime tuning base configuration does not match live boxes");
  }
  return absl::OkStatus();
}

absl::Status apply_runtime_tuning_to_tracker(
    DsPlayTrackerCtx::PlayTracker* tracker_context,
    const DsPlayTrackerRuntimeTuning& tuning) {
  if (!tracker_context || !tracker_context->play_tracker) {
    return absl::FailedPreconditionError("playtracker runtime tuning target is not initialized");
  }
  absl::Status status = validate_runtime_tuning_target(*tracker_context, tuning);
  if (!status.ok()) {
    return status;
  }
  auto* tracker = tracker_context->play_tracker.get();
  if (tuning.update_motion_tuning &&
      (tuning.overshoot_stop_delay_count.has_value() || tuning.overshoot_scale_speed_ratio.has_value())) {
    auto& applied = tracker_context->play_tracker_config.play_detector;
    tracker->set_breakaway_braking(
        tuning.overshoot_stop_delay_count.value_or(applied.overshoot_stop_delay_count),
        tuning.overshoot_scale_speed_ratio.value_or(applied.overshoot_scale_speed_ratio));
    if (tuning.overshoot_stop_delay_count.has_value()) {
      applied.overshoot_stop_delay_count = *tuning.overshoot_stop_delay_count;
    }
    if (tuning.overshoot_scale_speed_ratio.has_value()) {
      applied.overshoot_scale_speed_ratio = *tuning.overshoot_scale_speed_ratio;
    }
  }
  const size_t box_count = tracker_context->play_tracker_config.living_boxes.size();
  for (size_t index = 0; index < box_count; ++index) {
    const bool selected = (index == 0 && tuning.apply_to_fast_box) || (index == 1 && tuning.apply_to_follower_box);
    if (!selected) {
      continue;
    }
    auto& applied = tracker_context->play_tracker_config.living_boxes[index];
    const auto& base = tracker_context->base_play_tracker_config.living_boxes[index];
    auto box = tracker->get_live_box(index);
    if (tuning.update_motion_tuning) {
      if (tuning.stop_on_dir_change_delay.has_value() || tuning.cancel_on_opposite.has_value() ||
          tuning.cancel_hysteresis_frames.has_value() || tuning.stop_delay_cooldown_frames.has_value() ||
          tuning.post_nonstop_stop_delay_count.has_value() || tuning.time_to_dest_speed_limit_frames.has_value()) {
        box->set_braking(
            tuning.stop_on_dir_change_delay.value_or(applied.stop_translation_on_dir_change_delay),
            tuning.cancel_on_opposite.value_or(applied.cancel_stop_on_opposite_dir),
            tuning.cancel_hysteresis_frames.value_or(applied.cancel_stop_hysteresis_frames),
            tuning.stop_delay_cooldown_frames.value_or(applied.stop_delay_cooldown_frames),
            tuning.post_nonstop_stop_delay_count.value_or(applied.post_nonstop_stop_delay_count),
            tuning.time_to_dest_speed_limit_frames.value_or(applied.time_to_dest_speed_limit_frames));
        if (tuning.stop_on_dir_change_delay.has_value()) {
          applied.stop_translation_on_dir_change_delay = *tuning.stop_on_dir_change_delay;
        }
        if (tuning.cancel_on_opposite.has_value()) {
          applied.cancel_stop_on_opposite_dir = *tuning.cancel_on_opposite;
        }
        if (tuning.cancel_hysteresis_frames.has_value()) {
          applied.cancel_stop_hysteresis_frames = *tuning.cancel_hysteresis_frames;
        }
        if (tuning.stop_delay_cooldown_frames.has_value()) {
          applied.stop_delay_cooldown_frames = *tuning.stop_delay_cooldown_frames;
        }
        if (tuning.post_nonstop_stop_delay_count.has_value()) {
          applied.post_nonstop_stop_delay_count = *tuning.post_nonstop_stop_delay_count;
        }
        if (tuning.time_to_dest_speed_limit_frames.has_value()) {
          applied.time_to_dest_speed_limit_frames = *tuning.time_to_dest_speed_limit_frames;
        }
      }
      if (tuning.max_speed_x.has_value() || tuning.max_speed_y.has_value() || tuning.max_accel_x.has_value() ||
          tuning.max_accel_y.has_value()) {
        auto override_or_current = [](const std::optional<float>& value, float current_value, float base_value) {
          if (!value.has_value()) {
            return current_value;
          }
          return *value > 0.0f ? *value : base_value;
        };
        const float max_speed_x = override_or_current(tuning.max_speed_x, applied.max_speed_x, base.max_speed_x);
        const float max_speed_y = override_or_current(tuning.max_speed_y, applied.max_speed_y, base.max_speed_y);
        const float max_accel_x = override_or_current(tuning.max_accel_x, applied.max_accel_x, base.max_accel_x);
        const float max_accel_y = override_or_current(tuning.max_accel_y, applied.max_accel_y, base.max_accel_y);
        box->set_translation_constraints(max_speed_x, max_speed_y, max_accel_x, max_accel_y);
        applied.max_speed_x = max_speed_x;
        applied.max_speed_y = max_speed_y;
        applied.max_accel_x = max_accel_x;
        applied.max_accel_y = max_accel_y;
      }
    }
    if (tuning.arena_angle_from_vertical.has_value() || tuning.dynamic_acceleration_scaling.has_value()) {
      box->set_camera_geometry(
          tuning.arena_angle_from_vertical.value_or(applied.arena_angle_from_vertical),
          tuning.dynamic_acceleration_scaling.value_or(applied.dynamic_acceleration_scaling));
      if (tuning.arena_angle_from_vertical.has_value()) {
        applied.arena_angle_from_vertical = *tuning.arena_angle_from_vertical;
      }
      if (tuning.dynamic_acceleration_scaling.has_value()) {
        applied.dynamic_acceleration_scaling = *tuning.dynamic_acceleration_scaling;
      }
    }
  }
  return absl::OkStatus();
}

template <typename T>
void merge_optional(std::optional<T>* destination, const std::optional<T>& update) {
  if (update.has_value()) {
    *destination = update;
  }
}

void merge_detector_runtime_tuning(DsPlayTrackerRuntimeTuning* state, const DsPlayTrackerRuntimeTuning& update) {
  state->update_motion_tuning = true;
  state->apply_to_fast_box = false;
  state->apply_to_follower_box = false;
  merge_optional(&state->overshoot_stop_delay_count, update.overshoot_stop_delay_count);
  merge_optional(&state->overshoot_scale_speed_ratio, update.overshoot_scale_speed_ratio);
}

void merge_box_runtime_tuning(
    DsPlayTrackerRuntimeTuning* state,
    const DsPlayTrackerRuntimeTuning& update,
    bool fast_box) {
  state->update_motion_tuning = state->update_motion_tuning || update.update_motion_tuning;
  state->apply_to_fast_box = fast_box;
  state->apply_to_follower_box = !fast_box;
  merge_optional(&state->stop_on_dir_change_delay, update.stop_on_dir_change_delay);
  merge_optional(&state->cancel_on_opposite, update.cancel_on_opposite);
  merge_optional(&state->cancel_hysteresis_frames, update.cancel_hysteresis_frames);
  merge_optional(&state->stop_delay_cooldown_frames, update.stop_delay_cooldown_frames);
  merge_optional(&state->post_nonstop_stop_delay_count, update.post_nonstop_stop_delay_count);
  merge_optional(&state->time_to_dest_speed_limit_frames, update.time_to_dest_speed_limit_frames);
  merge_optional(&state->max_speed_x, update.max_speed_x);
  merge_optional(&state->max_speed_y, update.max_speed_y);
  merge_optional(&state->max_accel_x, update.max_accel_x);
  merge_optional(&state->max_accel_y, update.max_accel_y);
  if (update.arena_angle_from_vertical.has_value()) {
    state->arena_angle_from_vertical = update.arena_angle_from_vertical;
  }
  if (update.dynamic_acceleration_scaling.has_value()) {
    state->dynamic_acceleration_scaling = update.dynamic_acceleration_scaling;
  }
}

void accumulate_runtime_tuning(DsPlayTrackerCtx* ctx, const DsPlayTrackerRuntimeTuning& tuning) {
  if (tuning.overshoot_stop_delay_count.has_value() || tuning.overshoot_scale_speed_ratio.has_value()) {
    if (!ctx->detector_runtime_tuning.has_value()) {
      ctx->detector_runtime_tuning.emplace();
    }
    merge_detector_runtime_tuning(&*ctx->detector_runtime_tuning, tuning);
  }
  if (tuning.apply_to_fast_box) {
    if (!ctx->fast_box_runtime_tuning.has_value()) {
      ctx->fast_box_runtime_tuning.emplace();
    }
    merge_box_runtime_tuning(&*ctx->fast_box_runtime_tuning, tuning, true);
  }
  if (tuning.apply_to_follower_box) {
    if (!ctx->follower_box_runtime_tuning.has_value()) {
      ctx->follower_box_runtime_tuning.emplace();
    }
    merge_box_runtime_tuning(&*ctx->follower_box_runtime_tuning, tuning, false);
  }
}

absl::Status apply_accumulated_runtime_tuning(DsPlayTrackerCtx* ctx, DsPlayTrackerCtx::PlayTracker* tracker_context) {
  for (const auto* tuning :
       {&ctx->detector_runtime_tuning, &ctx->fast_box_runtime_tuning, &ctx->follower_box_runtime_tuning}) {
    if (tuning->has_value()) {
      absl::Status status = apply_runtime_tuning_to_tracker(tracker_context, **tuning);
      if (!status.ok()) {
        return status;
      }
    }
  }
  return absl::OkStatus();
}

bool has_play_tracker(DsPlayTrackerCtx* ctx, int source_id) {
  return !!ctx->play_trackers.count(source_id);
}

hm::play_tracker::PlayTracker* get_play_tracker(DsPlayTrackerCtx* ctx, int source_id) {
  return ctx->play_trackers.at(source_id).play_tracker.get();
}

hm::play_tracker::PlayTracker* get_or_create_play_tracker(DsPlayTrackerCtx* ctx, int source_id, const BBox& arena_box) {
  // std::cerr << "play tracker source_id = " << source_id << std::endl;
  if (has_play_tracker(ctx, source_id)) {
    return get_play_tracker(ctx, source_id);
  }
  if (!ctx->initParams.play_tracker_config_file.empty()) {
    try {
      YAML::Node yaml = YAML::LoadFile(ctx->initParams.play_tracker_config_file);
      if (yaml["play-tracker"]) {
        ctx->play_trackers[source_id].play_tracker_config = create_play_tracker_config(arena_box, yaml["play-tracker"]);
        ctx->play_trackers[source_id].base_play_tracker_config = ctx->play_trackers[source_id].play_tracker_config;
        ctx->play_trackers[source_id].play_tracker = std::make_unique<hm::play_tracker::PlayTracker>(
            arena_box, ctx->play_trackers[source_id].play_tracker_config);
        const absl::Status tuning_status = apply_accumulated_runtime_tuning(ctx, &ctx->play_trackers[source_id]);
        if (!tuning_status.ok()) {
          g_printerr("Could not apply accumulated playtracker runtime tuning: %s\n", tuning_status.ToString().c_str());
          ctx->play_trackers.erase(source_id);
          return nullptr;
        }
        return ctx->play_trackers[source_id].play_tracker.get();
      } else {
        g_error("Could not find 'play-tracker' in config file: %s", ctx->initParams.play_tracker_config_file.c_str());
        ctx->play_trackers[source_id].play_tracker = nullptr;
      }
    } catch (const std::exception& e) {
      g_error("Error loading YAML file: %s", e.what());
      ctx->play_trackers[source_id].play_tracker = nullptr;
    }
  } else {
    ctx->play_trackers[source_id].play_tracker = nullptr;
  }

  return nullptr;
}

void plot_progress_bar(
    hm::utils::PlotContext& plotter,
    const hm::BBox& bbox,
    float filled_ratio,
    const hm::utils::ColorRGB& line_color,
    const hm::utils::ColorRGB& unfilled_color,
    const hm::utils::ColorRGB& fill_color) {
  constexpr int kThickness = 1;
  plotter.plot_rect(bbox, /*thickness=*/kThickness, line_color, /*fill_color=*/unfilled_color);
  hm::BBox inner_rect = bbox.deflate(kThickness, kThickness);
  hm::FloatValue ww = inner_rect.width() * std::abs(filled_ratio);
  inner_rect.right = std::min(inner_rect.right, inner_rect.left + ww);
  if (inner_rect.width() <= 0 || inner_rect.height() <= 0) {
    return;
  }
  plotter.plot_rect(inner_rect, /*thickness=*/0, fill_color, fill_color);
}

void plot_resizing_state(
    hm::utils::PlotContext& plotter,
    const ILivingBox* lbox,
    const hm::play_tracker::AllLivingBoxConfig& box_config,
    bool draw_thresholds,
    double scale_width,
    double scale_height,
    std::optional<ILivingBox*> following_lbox = std::nullopt) {
  const hm::play_tracker::ResizingState& resizing_state = lbox->get_resizing_state();
  if (box_config.sticky_sizing) {
    BBox my_bbox = lbox->bounding_box().make_canvas_scaled(scale_width, scale_height);
    assert(following_lbox.has_value());
    BBox following_box = following_lbox.value()->bounding_box().make_canvas_scaled(scale_width, scale_height);
    if (resizing_state.size_is_frozen) {
      // Draw thick corners when frozen
      plotter.plot_corner_rect(my_bbox, /*thickness=*/8, hm::utils::ColorRGB{255, 255, 255}, 0.2, 0.2);
      // BBox corner_box = my_bbox.make_scaled(0.98, 0.98);
      // plotter.plot_corner_rect(corner_box, /*thickness=*/8, hm::utils::ColorRGB{255, 255, 255}, 0.2, 0.2);
    }
    BBox scaled_following_box = following_box.make_scaled(box_config.scale_dest_width, box_config.scale_dest_height);
    BBox inscribed = scaled_following_box.at_center(my_bbox.center());
    int dash_length = inscribed.width() / 5;
    plotter.plot_dashed_rect(
        inscribed,
        /*thickness=*/2,
        hm::utils::ColorRGB{255, 255, 255},
        /*dash_length=*/dash_length,
        /*gap_length=*/dash_length);
    if (draw_thresholds) {
      Point my_center = my_bbox.center();
      auto my_width = my_bbox.width(), my_height = my_bbox.height();
      hm::play_tracker::GrowShrink gs = lbox->get_grow_shrink_wh(my_bbox);
      BBox grow_box =
          BBox(my_center, hm::WHDims{.width = my_width + gs.grow_width, .height = my_height + gs.grow_height});
      BBox shrink_box =
          BBox(my_center, hm::WHDims{.width = my_width - gs.shrink_width, .height = my_height - gs.shrink_height});
      plotter.plot_no_corner_rect(grow_box, /*thickness=*/4, hm::utils::ColorRGB{0, 255, 0}, 0.5, 0.5);
      plotter.plot_no_corner_rect(shrink_box, /*thickness=*/4, hm::utils::ColorRGB{0, 0, 255}, 0.5, 0.5);
    }
  }
}

void plot_translation_state(
    hm::utils::PlotContext& plotter,
    const ILivingBox* lbox,
    const hm::play_tracker::AllLivingBoxConfig& box_config,
    int thickness,
    const hm::utils::ColorT& color,
    bool draw_thresholds,
    double scale_width,
    double scale_height,
    std::optional<ILivingBox*> following_lbox = std::nullopt) {
  const hm::play_tracker::TranslationState& translation_state = lbox->get_translation_state();
  // (void)translation_state;
  BBox my_bbox = lbox->bounding_box().make_canvas_scaled(scale_width, scale_height);
  plotter.plot_rect(
      my_bbox, thickness, translation_state.translation_is_frozen ? hm::utils::ColorRGB{128, 128, 128} : color);
  if (draw_thresholds && box_config.sticky_translation) {
    const auto sticky_unsticky = lbox->get_sticky_translation_sizes();
    const float scale_ratio = std::sqrt(scale_width * scale_height + scale_width * scale_height);
    const float sticky = std::get<0>(sticky_unsticky) * scale_ratio;
    const float unsticky = std::get<1>(sticky_unsticky) * scale_ratio;
    Point my_center = my_bbox.center();
    plotter.plot_circle(my_center, /*radius=*/int(sticky), /*thickness=*/3, hm::utils::ColorRGB{255, 0, 0});
    plotter.plot_circle(my_center, /*radius=*/int(unsticky), /*thickness=*/3, hm::utils::ColorRGB{255, 0, 255});
    if (following_lbox.has_value()) {
      BBox following_bbox = (*following_lbox)->bounding_box().make_canvas_scaled(scale_width, scale_height);
      Point following_bbox_center = following_bbox.center();
      plotter.plot_circle(
          my_center, /*radius=*/5, /*thickness=*/1, hm::utils::ColorRGB{255, 255, 0}, hm::utils::ColorRGB{255, 255, 0});
      plotter.plot_circle(
          following_bbox_center,
          /*radius=*/5,
          /*thickness=*/1,
          hm::utils::ColorRGB{0, 255, 128},
          hm::utils::ColorRGB{0, 255, 128});
      // Diagonal
      plotter.plot_line(my_center, following_bbox_center, /*thickness=*/10, hm::utils::ColorRGB{255, 0, 0});
      // X shaft
      plotter.plot_line(
          my_center,
          Point{.x = following_bbox_center.x, .y = my_center.y},
          /*thickness=*/3,
          hm::utils::ColorRGB{255, 255, 0});
      // Y shaft
      plotter.plot_line(
          my_center,
          Point{.x = my_center.x, .y = following_bbox_center.y},
          /*thickness=*/3,
          hm::utils::ColorRGB{255, 255, 0});
      // Translation edge scale
      hm::BBox prog(my_center, hm::WHDims{.width = sticky / 2, .height = 25});
      plot_progress_bar(
          plotter,
          prog,
          translation_state.last_arena_edge_center_position_scale,
          hm::utils::ColorRGB{128, 128, 128},
          hm::utils::ColorRGB{64, 64, 64},
          hm::utils::ColorRGB{128, 255, 255});
    }
  }
}

void plot_living_box(
    hm::utils::PlotContext& plotter,
    const ILivingBox* lbox,
    const hm::play_tracker::AllLivingBoxConfig& box_config,
    int thickness,
    const hm::utils::ColorT& color,
    bool draw_thresholds,
    double scale_width,
    double scale_height,
    std::optional<ILivingBox*> following_lbox = std::nullopt) {
  plot_translation_state(
      plotter, lbox, box_config, thickness, color, draw_thresholds, scale_width, scale_height, following_lbox);
  plot_resizing_state(plotter, lbox, box_config, draw_thresholds, scale_width, scale_height, following_lbox);
}

const std::array<hm::utils::ColorRGB, 2> track_colors{
    hm::utils::ColorRGB{0, 0, 255},
    hm::utils::ColorRGB{255, 0, 255},
};
const hm::utils::ColorRGB breakway_edge_line{128, 0, 28};
const hm::utils::ColorRGB breakway_edge_circle{128, 0, 28};

} // namespace gst_hm_playtracker

namespace {
struct ScaleXY {
  double scale_x{1.0};
  double scale_y{1.0};
};

ScaleXY get_scale_xy(const GstDsPlayTrackerFrame& frame) {
  double pipeline_width = frame.input_surf_params->width;
  double pipeline_height = frame.input_surf_params->height;
  const double scale_x = double(frame.frame_meta->source_frame_width) / pipeline_width;
  const double scale_y = double(frame.frame_meta->source_frame_height) / pipeline_height;
  return ScaleXY{.scale_x = scale_x, .scale_y = scale_y};
}

} // namespace

DsPlayTrackerCtx* DsPlayTrackerCtxInit(DsPlayTrackerInitParams* initParams) {
  DsPlayTrackerCtx* ctx = new DsPlayTrackerCtx();
  ctx->initParams = *initParams;
  return ctx;
}

absl::Status DsPlayTrackerValidateConfigFile(const std::string& config_file) {
  if (config_file.empty()) {
    return absl::InvalidArgumentError("playtracker config-file is empty");
  }
  try {
    YAML::Node yaml = YAML::LoadFile(config_file);
    if (!yaml["play-tracker"]) {
      return absl::InvalidArgumentError(absl::StrCat("missing play-tracker in config file: ", config_file));
    }
    YAML::Node live_boxes = yaml["play-tracker"]["live-boxes"];
    if (!live_boxes || !live_boxes.IsSequence() || live_boxes.size() == 0) {
      return absl::InvalidArgumentError("playtracker config play-tracker.live-boxes must be a non-empty sequence");
    }
    std::string numeric_error;
    if (!gst_hm_playtracker::validate_numeric_yaml_fields(yaml["play-tracker"], &numeric_error)) {
      return absl::InvalidArgumentError(numeric_error);
    }
    const YAML::Node play_tracker = yaml["play-tracker"];
    if (play_tracker["camera-name"] &&
        !gst_hm_playtracker::CAMERA_TYPE_MAX_SPEEDS.count(play_tracker["camera-name"].as<std::string>())) {
      return absl::InvalidArgumentError("unsupported playtracker camera-name");
    }
    for (const char* key : {
             "follower-box-min-height-ratio",
             "max-accel-ratio-x",
             "max-accel-ratio-y",
             "max-speed-ratio-x",
             "max-speed-ratio-y",
         }) {
      if (play_tracker[key] && play_tracker[key].as<double>() < 0.0)
        return absl::InvalidArgumentError(absl::StrCat("playtracker ", key, " must be non-negative"));
    }
    (void)gst_hm_playtracker::create_play_tracker_config(hm::BBox(0, 0, 1920, 1080), yaml["play-tracker"]);
  } catch (const std::exception& exc) {
    return absl::InvalidArgumentError(absl::StrCat("invalid playtracker config file: ", exc.what()));
  }
  return absl::OkStatus();
}

absl::StatusOr<DsPlayTrackerRuntimeTuning> DsPlayTrackerLoadRuntimeTuning(const std::string& config_file) {
  absl::Status status = DsPlayTrackerValidateConfigFile(config_file);
  if (!status.ok()) {
    return status;
  }
  try {
    const YAML::Node play_tracker = YAML::LoadFile(config_file)["play-tracker"];
    const YAML::Node runtime = play_tracker["hstream-runtime-tuning"];
    auto read_int = [](const YAML::Node& node, const char* key) -> std::optional<int> {
      return node[key] && node[key].IsScalar() ? std::optional<int>(node[key].as<int>()) : std::nullopt;
    };
    auto read_float = [](const YAML::Node& node, const char* key) -> std::optional<float> {
      return node[key] && node[key].IsScalar() ? std::optional<float>(node[key].as<float>()) : std::nullopt;
    };
    auto read_bool = [](const YAML::Node& node, const char* key, bool fallback) {
      return node[key] && node[key].IsScalar() ? node[key].as<bool>() : fallback;
    };
    const bool apply_to_fast = read_bool(play_tracker, "hstream-apply-to-fast-box", false);
    const bool apply_to_follower = read_bool(play_tracker, "hstream-apply-to-follower-box", true);
    const YAML::Node live_boxes = play_tracker["live-boxes"];
    const size_t live_box_count = live_boxes && live_boxes.IsSequence() ? live_boxes.size() : 0;
    if (apply_to_fast && live_box_count < 1) {
      return absl::FailedPreconditionError("playtracker runtime tuning requires a fast live box");
    }
    if (apply_to_follower && live_box_count < 2) {
      return absl::FailedPreconditionError("playtracker runtime tuning requires a follower live box");
    }
    return DsPlayTrackerRuntimeTuning{
        .stop_on_dir_change_delay = read_int(runtime, "stop-translation-on-dir-change-delay"),
        .cancel_on_opposite =
            runtime["cancel-stop-on-opposite-dir"] && runtime["cancel-stop-on-opposite-dir"].IsScalar()
            ? std::optional<bool>(runtime["cancel-stop-on-opposite-dir"].as<bool>())
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
        .apply_to_fast_box = apply_to_fast,
        .apply_to_follower_box = apply_to_follower,
        .arena_angle_from_vertical = read_float(runtime, "arena-angle-from-vertical"),
        .dynamic_acceleration_scaling = read_float(runtime, "dynamic-acceleration-scaling"),
    };
  } catch (const std::exception& exc) {
    return absl::InvalidArgumentError(absl::StrCat("invalid playtracker runtime config: ", exc.what()));
  }
}

absl::Status DsPlayTrackerCtxApplyRuntimeTuning(DsPlayTrackerCtx* ctx, const DsPlayTrackerRuntimeTuning& tuning) {
  if (!ctx) {
    return absl::InvalidArgumentError("playtracker context is null");
  }
  std::vector<std::pair<hm::play_tracker::PlayTracker*, DsPlayTrackerCtx::PlayTracker*>> targets;
  for (auto& [source_id, tracker_context] : ctx->play_trackers) {
    (void)source_id;
    auto* tracker = tracker_context.play_tracker.get();
    if (!tracker) {
      continue;
    }
    targets.emplace_back(tracker, &tracker_context);
  }
  // Validate every target before mutating any tracker so a rejected update
  // cannot leave only some sources changed.
  for (const auto& [tracker, tracker_context] : targets) {
    (void)tracker;
    absl::Status status = gst_hm_playtracker::validate_runtime_tuning_target(*tracker_context, tuning);
    if (!status.ok()) {
      return status;
    }
  }
  for (const auto& [tracker, tracker_context] : targets) {
    (void)tracker;
    absl::Status status = gst_hm_playtracker::apply_runtime_tuning_to_tracker(tracker_context, tuning);
    if (!status.ok()) {
      return status;
    }
  }
  gst_hm_playtracker::accumulate_runtime_tuning(ctx, tuning);
  return absl::OkStatus();
}

bool DsPlayTrackerProcessFrame(DsPlayTrackerCtx* ctx, GstDsPlayTrackerFrame& frame, cudaStream_t stream) {
  // We always do our calculations wrt the original image, since we tune based upon the camera
  // type, which is generally tied to the resolution. We scale in the play tracker when possible, but
  // it isn't perfectly scalable atm.

  DsPlayTrackerCtx::PlayTracker* play_tracker_ctx{nullptr};

#if 1 && !defined(NDEBUG)
  hm::utils::PlotContext plot_context(frame.frame_meta);
  plot_context.plot_rect(
      // hm::BBox(field_box.x, field_box.y, field_box.x + field_box.width, field_box.y + field_box.height),
      ctx->arena_box,
      20,
      hm::utils::ColorRGB{255, 0, 0});
#endif

  if (!gst_hm_playtracker::has_play_tracker(ctx, frame.frame_meta->source_id)) {
    ctx->arena_box = hm::BBox(0, 0, frame.frame_meta->source_frame_width, frame.frame_meta->source_frame_height);
#ifdef HAS_NVDS_CUSTOMUSERMETA
    const hm::fieldmask::FieldMaskPayload* fieldmask_payload =
        hm::fieldmask::FieldMaskPayload::get_payload<hm::fieldmask::FieldMaskPayload>(frame.frame_meta);
    if (fieldmask_payload) {
      const cv::Rect2i& field_box = fieldmask_payload->field_box();
      float horizontal_expand_ratio = 0.04;
      float horizontal_padding = horizontal_expand_ratio * field_box.width;
      float new_left = field_box.x - horizontal_padding;
      if (new_left < ctx->arena_box.left) {
        new_left = ctx->arena_box.left;
      }
      float new_right = (field_box.x + field_box.width) + horizontal_padding;
      if (new_right > ctx->arena_box.right) {
        new_right = ctx->arena_box.right;
      }
      // Inflate and only apply left and right
      ctx->arena_box = hm::BBox(new_left, ctx->arena_box.top, new_right, ctx->arena_box.bottom);
#if 0 && !defined(NDEBUG)
      plot_context.plot_rect(
          ctx->arena_box,
          20,
          hm::utils::ColorRGB{0, 255, 128});
#endif
    }
#endif
    gst_hm_playtracker::get_or_create_play_tracker(ctx, frame.frame_meta->source_id, ctx->arena_box);
    play_tracker_ctx = &ctx->play_trackers.at(frame.frame_meta->source_id);
  } else {
    play_tracker_ctx = &ctx->play_trackers.at(frame.frame_meta->source_id);
  }
  if (!play_tracker_ctx || !play_tracker_ctx->play_tracker) {
    return false;
  }
  hm::play_tracker::PlayTracker* play_tracker = play_tracker_ctx->play_tracker.get();

  std::vector<size_t> tracking_ids;
  std::vector<hm::BBox> tracking_boxes;

  const std::size_t object_count = g_list_length(frame.frame_meta->obj_meta_list);
  tracking_ids.reserve(object_count);
  tracking_boxes.reserve(object_count);

  // assert(frame.frame_meta->pipeline_width && frame.frame_meta->pipeline_height);

  ScaleXY scale_xy = get_scale_xy(frame);
  const auto& scale_x = scale_xy.scale_x;
  const auto& scale_y = scale_xy.scale_y;

  for (NvDsMetaList* l_obj = frame.frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
    NvDsObjectMeta* obj_meta = (NvDsObjectMeta*)(l_obj->data);
    if (obj_meta->class_id != 0) {
      continue;
    }
    if (obj_meta->object_id == UNTRACKED_OBJECT_ID) {
      // ignore untracked objects
      continue;
    }
    const NvDsComp_BboxInfo& tracker_bbox_info = obj_meta->tracker_bbox_info;
    tracking_boxes.emplace_back(
        hm::BBox(
            tracker_bbox_info.org_bbox_coords.left * scale_x,
            tracker_bbox_info.org_bbox_coords.top * scale_y,
            (tracker_bbox_info.org_bbox_coords.left + tracker_bbox_info.org_bbox_coords.width) * scale_x,
            (tracker_bbox_info.org_bbox_coords.top + tracker_bbox_info.org_bbox_coords.height) * scale_y));
    size_t tracking_id = obj_meta->object_id;
    tracking_ids.push_back(tracking_id);
  }

  if (tracking_boxes.empty() && !play_tracker_ctx->has_received_tracks) {
    // Keep the frame and defer tracker initialization until real tracks
    // arrive. Empty results intentionally leave playcropper on its centered,
    // aspect-correct fallback instead of stretching a synthetic full-frame
    // box into the Program output.
    frame.play_tracker_results = {};
    return true;
  }

  if (!tracking_boxes.empty()) {
    play_tracker_ctx->has_received_tracks = true;
  }

  frame.play_tracker_results = play_tracker->forward(tracking_ids, tracking_boxes);
  if (ctx->initParams.draw) {
    if (!DsPlayTrackerDrawToDisplayMeta(ctx, frame).ok()) {
      return false;
    }
  }
  DsPlayTrackerAttachMetadataFullFrame(frame.frame_meta, frame.play_tracker_results);
  return true;
}

absl::Status DsPlayTrackerDrawToDisplayMeta(DsPlayTrackerCtx* ctx, GstDsPlayTrackerFrame& frame) {
  ScaleXY scale_xy = get_scale_xy(frame);
  const auto& scale_x = scale_xy.scale_x;
  const auto& scale_y = scale_xy.scale_y;

  hm::utils::PlotContext plotter(frame.frame_meta, "");
  // Plot any nontrivial arena box
  if (ctx->arena_box.left > 0 || ctx->arena_box.top > 0 ||
      ctx->arena_box.width() < frame.frame_meta->source_frame_width ||
      ctx->arena_box.height() < frame.frame_meta->source_frame_height) {
    plotter.plot_rect(ctx->arena_box, 2, hm::utils::ColorRGBA{255, 64, 64, 255});
  }
  for (const auto& cluster_item : frame.play_tracker_results.cluster_boxes) {
    plotter.plot_rect(
        cluster_item.second.make_canvas_scaled(1.0 / scale_x, 1.0 / scale_y),
        1,
        hm::utils::ColorRGB{0, 0, 0},
        hm::utils::ColorRGBA{128, 128, 128, 75});
  }
  for (size_t i = 0, n = frame.play_tracker_results.tracking_boxes.size(); i < n; ++i) {
    // plotter.plot_rect(frame.play_tracker_results.tracking_boxes[i], 5, track_colors.at(i));
    auto& play_tracker_ctx = ctx->play_trackers[frame.frame_meta->source_id];
    if (play_tracker_ctx.play_tracker) {
      std::shared_ptr<hm::play_tracker::ILivingBox> lbox = play_tracker_ctx.play_tracker->get_live_box(i);
      hm::play_tracker::ILivingBox* following_box =
          i ? play_tracker_ctx.play_tracker->get_live_box(i - 1).get() : nullptr;

      // We scale back down for drawing, which is on the pipeline image

      gst_hm_playtracker::plot_living_box(
          plotter,
          lbox.get(),
          play_tracker_ctx.play_tracker_config.living_boxes.at(i),
          /*thickness=*/4,
          gst_hm_playtracker::track_colors.at(i),
          /*draw_thresholds=*/true,
          1.0 / scale_x,
          1.0 / scale_y,
          following_box);
    }
  }
  if (frame.play_tracker_results.play_detection.has_value()) {
    const hm::play_tracker::PlayDetectorResults& play_detector = *frame.play_tracker_results.play_detection;
    if (play_detector.breakaway_edge_center.has_value()) {
      plotter.plot_circle(
          *play_detector.breakaway_edge_center,
          /*radius=*/30,
          /*thickness=*/15,
          gst_hm_playtracker::breakway_edge_circle);
      plotter.plot_line(
          frame.play_tracker_results.tracking_boxes.at(0).center(),
          *play_detector.breakaway_edge_center,
          3,
          gst_hm_playtracker::breakway_edge_line);
    }
  }
  // Finally, print the translation scaling value
  // frame.play_tracker_results.
  return absl::OkStatus();
}

/**
 * Attach metadata for the full frame. We will be adding a new metadata.
 */
void DsPlayTrackerAttachMetadataFullFrame(
    NvDsFrameMeta* frame_meta,
    const hm::play_tracker::PlayTrackerResults& play_results) {
  NvDsBatchMeta* batch_meta = frame_meta->base_meta.batch_meta;
  NvDsObjectMeta* object_meta = NULL;

  size_t adder = 0;
  // Start with base vlass id being the last following box
  for (int64_t i = play_results.tracking_boxes.size() - 1; i >= 0; --i, ++adder) {
    const hm::BBox& tracking_box = play_results.tracking_boxes[i];
    object_meta = nvds_acquire_obj_meta_from_pool(batch_meta);
    object_meta->class_id = DsPlayTrackerInitParams::kPlayBoxClassIdBase + adder;

    NvOSD_RectParams& rect_params = object_meta->rect_params;

    // Assign bounding box coordinates
    rect_params.left = tracking_box.left;
    rect_params.top = tracking_box.top;
    rect_params.width = tracking_box.width();
    rect_params.height = tracking_box.height();

    rect_params.border_width = 0;
    rect_params.border_color = (NvOSD_ColorParams){1, 1, 0, 1};

    object_meta->object_id = UNTRACKED_OBJECT_ID;

    nvds_add_obj_meta_to_frame(frame_meta, object_meta, NULL);
  }
}

void DsPlayTrackerCtxDeinit(DsPlayTrackerCtx* ctx) {
  if (ctx) {
    ctx->play_trackers.clear();
    delete ctx;
  }
}
