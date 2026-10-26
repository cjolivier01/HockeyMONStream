#include "hstream/src/libs/tracking/ProgramCameraConfig.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "absl/strings/str_cat.h"
#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerRuntimeConfig.h"
#include "hstream/src/libs/common/ConfigYaml.h"
#include "hstream/src/libs/common/PlayTrackerConfigRoles.h"

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
      "zoom-in-aggressiveness",
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

hm::BBox program_camera_arena(const hm::BBox& frame, const hm::BBox& field) {
  // Existing Program rule: extend the ice bounds horizontally by four percent,
  // while retaining the full source height for native camera motion.
  const float padding = field.width() * 0.04f;
  return hm::BBox(
      std::max(frame.left, field.left - padding),
      frame.top,
      std::min(frame.right, field.right + padding),
      frame.bottom);
}

bool supported_camera(const std::string& name) {
  return CAMERA_TYPE_MAX_SPEEDS.count(name) != 0;
}

void adjust_config(const BBox& arena_box, PlayTrackerConfig& pt_config, const std::string& camera) {
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
          "zoom-in-aggressiveness",
          "oversized-bbox-percent",
      },
  };
  std::vector<YAML::Node> live_box_yamls;

  if (yaml["live-boxes"]) {
    YAML::Node live_boxes = yaml["live-boxes"];
    const auto normalized_order = hm::normalized_playtracker_live_box_order(live_boxes);
    if (!normalized_order.ok())
      throw std::invalid_argument(normalized_order.status().ToString());
    for (const size_t index : *normalized_order) {
      const YAML::Node box_yaml = live_boxes[index];
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

  const std::string camera_name = yaml["camera-name"].as<std::string>();
  adjust_config(arena_box, config, camera_name);
  const float max_speed_ratio_x = yaml["max-speed-ratio-x"].as<float>();
  const float max_speed_ratio_y = yaml["max-speed-ratio-y"].as<float>();
  const float max_accel_ratio_x = yaml["max-accel-ratio-x"].as<float>();
  const float max_accel_ratio_y = yaml["max-accel-ratio-y"].as<float>();
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
  if (!config.living_boxes.empty()) {
    config.living_boxes.back().min_height = arena_box.height() * yaml["follower-box-min-height-ratio"].as<float>();
  }
  for (size_t i = 0; i < live_box_yamls.size() && i < config.living_boxes.size(); ++i) {
    const std::optional<FloatValue> fixed_aspect_ratio =
        i + 1 == config.living_boxes.size() ? std::optional<FloatValue>(16.0 / 9.0) : std::nullopt;
    apply_all_living_box_config(arena_box, config.living_boxes[i], live_box_yamls[i], fixed_aspect_ratio);
  }
  // The upstream config stores this flag as int. Use a bool locator to accept
  // YAML true/false as well as legacy 0/1 without changing integer parsing.
  bool no_wide_start = config.no_wide_start;
  locator.locators["no_wide_start"] = &no_wide_start;
  SET_LOCATOR(locator, config, max_lost_track_age);
  SET_LOCATOR(locator, config, ignore_largest_bbox);
  SET_LOCATOR(locator, config, ignore_largest_bbox_count);
  SET_LOCATOR(locator, config, ignore_oversized_bboxes);
  if (yaml["oversized-bbox-percent"])
    config.oversized_bbox_percent = yaml["oversized-bbox-percent"].as<double>();
  // These wrapper-owned fields filter observations before the camera engine.
  auto native_yaml = YAML::Clone(yaml);
  native_yaml.remove("ignore-referees");
  native_yaml.remove("ignore-goalies");
  native_yaml.remove("person-roles");
  set_config_from_yaml(native_yaml, locator);
  config.no_wide_start = no_wide_start;
  if (config.ignore_largest_bbox_count < 0 || !std::isfinite(config.oversized_bbox_percent) ||
      config.oversized_bbox_percent < 0)
    throw std::invalid_argument("Player size count and percentage must be finite and nonnegative");

  const int zoom_in_aggressiveness =
      yaml["zoom-in-aggressiveness"] ? yaml["zoom-in-aggressiveness"].as<int>() : kDefaultZoomInAggressiveness;
  if (zoom_in_aggressiveness < kMinimumZoomInAggressiveness || zoom_in_aggressiveness > kMaximumZoomInAggressiveness) {
    throw std::invalid_argument("zoom-in-aggressiveness must be from 0 through 100");
  }
  if (!config.living_boxes.empty()) {
    AllLivingBoxConfig& follower = config.living_boxes.back();
    const float multiplier = DsPlayTrackerZoomInThresholdMultiplier(zoom_in_aggressiveness);
    follower.size_ratio_thresh_shrink_dw *= multiplier;
    follower.size_ratio_thresh_shrink_dh *= multiplier;
  }

  return config;
}

} // namespace gst_hm_playtracker
