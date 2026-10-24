#include "hstream/src/libs/tracking/ProgramCameraMapping.h"

#include <cmath>
#include <optional>
#include <vector>

#include "hstream/src/libs/common/ConfigPath.h"
#include "hstream/src/libs/common/PlayTrackerConfigRoles.h"
#include "hstream/src/libs/common/StatusMacros.h"

namespace hm::tracking {
absl::StatusOr<YAML::Node> ResolveProgramCameraConfig(
    const YAML::Node& effective_config,
    const ConfigLeafRanks& canonical_value_ranks,
    int native_base_rank,
    const YAML::Node& base_playtracker_config) {
  if (!effective_config.IsMap())
    return absl::InvalidArgumentError("Effective application config must be a YAML map");
  if (native_base_rank < 0)
    return absl::InvalidArgumentError("Native playtracker provenance rank must be non-negative");
  if (!base_playtracker_config.IsMap() || !base_playtracker_config["play-tracker"].IsMap())
    return absl::InvalidArgumentError("Playtracker config must contain a play-tracker map");

  YAML::Node result = YAML::Clone(base_playtracker_config);
  YAML::Node play_tracker = result["play-tracker"];
  YAML::Node live_boxes = play_tracker["live-boxes"];
  if (!live_boxes.IsSequence() || live_boxes.size() == 0)
    return absl::InvalidArgumentError("Playtracker config must contain at least one live-box");
  PlayTrackerLiveBoxRoles roles;
  HM_ASSIGN_OR_RETURN(roles, resolve_playtracker_live_box_roles(live_boxes));

  enum class ScalarType { kString, kBool, kInt, kDouble };
  auto validate_scalar = [](const YAML::Node& value, ScalarType type, const std::string& path) -> absl::Status {
    if (!value.IsDefined() || value.IsNull() || !value.IsScalar())
      return absl::InvalidArgumentError(path + " must be a non-null scalar");
    try {
      switch (type) {
        case ScalarType::kString:
          (void)value.as<std::string>();
          break;
        case ScalarType::kBool:
          (void)value.as<bool>();
          break;
        case ScalarType::kInt:
          (void)value.as<int>();
          break;
        case ScalarType::kDouble: {
          const double parsed = value.as<double>();
          if (!std::isfinite(parsed))
            return absl::InvalidArgumentError(path + " must be finite");
          break;
        }
      }
    } catch (const YAML::Exception& error) {
      return absl::InvalidArgumentError("Invalid " + path + ": " + error.what());
    }
    return absl::OkStatus();
  };

  auto copy_required = [&](YAML::Node destination,
                           const char* destination_key,
                           const char* source_path,
                           ScalarType type,
                           const std::string& destination_path) {
    const std::optional<YAML::Node> source = get_node(effective_config, source_path);
    if (!source || !source->IsDefined() || source->IsNull())
      return absl::InvalidArgumentError(std::string("Effective baseline is missing required key ") + source_path);
    const YAML::Node current = destination[destination_key];
    const auto source_rank_it = canonical_value_ranks.find(source_path);
    const int source_rank = source_rank_it == canonical_value_ranks.end() ? 0 : source_rank_it->second;
    if (!current.IsDefined() || current.IsNull() || source_rank > native_base_rank) {
      HM_RETURN_IF_ERROR(validate_scalar(*source, type, source_path));
      destination[destination_key] = YAML::Clone(*source);
    } else {
      HM_RETURN_IF_ERROR(validate_scalar(current, type, destination_path));
    }
    return absl::OkStatus();
  };

  auto copy_global = [&](const char* destination_key, const char* source_path, ScalarType type) {
    return copy_required(
        play_tracker, destination_key, source_path, type, std::string("play-tracker.") + destination_key);
  };

  HM_RETURN_IF_ERROR(copy_global("camera-name", "camera.name", ScalarType::kString));
  HM_RETURN_IF_ERROR(copy_global("no-wide-start", "play_tracker.no_wide_start", ScalarType::kBool));
  HM_RETURN_IF_ERROR(copy_global("ignore-largest-bbox", "rink.tracking.cam_ignore_largest", ScalarType::kBool));
  HM_RETURN_IF_ERROR(
      copy_global("ignore-largest-bbox-count", "rink.tracking.cam_ignore_largest_count", ScalarType::kInt));
  HM_RETURN_IF_ERROR(copy_global("ignore-oversized-bboxes", "rink.tracking.cam_ignore_oversized", ScalarType::kBool));
  HM_RETURN_IF_ERROR(copy_global("oversized-bbox-percent", "rink.tracking.cam_oversized_percent", ScalarType::kDouble));
  if (play_tracker["ignore-largest-bbox-count"].as<int>() < 0 ||
      play_tracker["oversized-bbox-percent"].as<double>() < 0)
    return absl::InvalidArgumentError("Player size count and percentage must be nonnegative");
  HM_RETURN_IF_ERROR(copy_required(
      play_tracker,
      "min-considered-group-velocity",
      "rink.camera.breakaway_detection.min_considered_group_velocity",
      ScalarType::kDouble,
      "play-tracker.min-considered-group-velocity"));
  HM_RETURN_IF_ERROR(copy_global(
      "group-ratio-threshold", "rink.camera.breakaway_detection.group_ratio_threshold", ScalarType::kDouble));
  HM_RETURN_IF_ERROR(copy_required(
      play_tracker,
      "group-velocity-speed-ratio",
      "rink.camera.breakaway_detection.group_velocity_speed_ratio",
      ScalarType::kDouble,
      "play-tracker.group-velocity-speed-ratio"));
  HM_RETURN_IF_ERROR(copy_required(
      play_tracker,
      "scale-speed-constraints",
      "rink.camera.breakaway_detection.scale_speed_constraints",
      ScalarType::kDouble,
      "play-tracker.scale-speed-constraints"));
  HM_RETURN_IF_ERROR(
      copy_global("nonstop-delay-count", "rink.camera.breakaway_detection.nonstop_delay_count", ScalarType::kInt));
  HM_RETURN_IF_ERROR(copy_required(
      play_tracker,
      "overshoot-scale-speed-ratio",
      "rink.camera.breakaway_detection.overshoot_scale_speed_ratio",
      ScalarType::kDouble,
      "play-tracker.overshoot-scale-speed-ratio"));
  HM_RETURN_IF_ERROR(copy_required(
      play_tracker,
      "overshoot-stop-delay-count",
      "rink.camera.breakaway_detection.overshoot_stop_delay_count",
      ScalarType::kInt,
      "play-tracker.overshoot-stop-delay-count"));
  HM_RETURN_IF_ERROR(copy_global("max-speed-ratio-x", "rink.camera.max_speed_ratio_x", ScalarType::kDouble));
  HM_RETURN_IF_ERROR(copy_global("max-speed-ratio-y", "rink.camera.max_speed_ratio_y", ScalarType::kDouble));
  HM_RETURN_IF_ERROR(copy_global("max-accel-ratio-x", "rink.camera.max_accel_ratio_x", ScalarType::kDouble));
  HM_RETURN_IF_ERROR(copy_global("max-accel-ratio-y", "rink.camera.max_accel_ratio_y", ScalarType::kDouble));
  HM_RETURN_IF_ERROR(
      copy_global("follower-box-min-height-ratio", "rink.camera.follower_box_min_height_ratio", ScalarType::kDouble));
  HM_RETURN_IF_ERROR(copy_global("zoom-in-aggressiveness", "rink.camera.zoom_in_aggressiveness", ScalarType::kInt));
  const int zoom_in_aggressiveness = play_tracker["zoom-in-aggressiveness"].as<int>();
  if (zoom_in_aggressiveness < 0 || zoom_in_aggressiveness > 100) {
    return absl::InvalidArgumentError("rink.camera.zoom_in_aggressiveness must be from 0 through 100");
  }

  for (size_t index = 0; index < live_boxes.size(); ++index) {
    YAML::Node box = live_boxes[index];
    const YAML::Node name = box["name"];
    if (name.IsDefined() && !name.IsNull()) {
      HM_RETURN_IF_ERROR(validate_scalar(name, ScalarType::kString, "play-tracker.live-boxes.name"));
    }
    const std::string box_path = "play-tracker.live-boxes[" + std::to_string(index) + "].";
    HM_RETURN_IF_ERROR(copy_required(
        box,
        "time-to-dest-speed-limit-frames",
        "rink.camera.time_to_dest_speed_limit_frames",
        ScalarType::kInt,
        box_path + "time-to-dest-speed-limit-frames"));
    HM_RETURN_IF_ERROR(copy_required(
        box,
        "time-to-dest-stop-speed-threshold",
        "rink.camera.time_to_dest_stop_speed_threshold",
        ScalarType::kDouble,
        box_path + "time-to-dest-stop-speed-threshold"));
    HM_RETURN_IF_ERROR(copy_required(
        box,
        "resizing-stop-on-dir-change-delay",
        "rink.camera.resizing_stop_on_dir_change_delay",
        ScalarType::kInt,
        box_path + "resizing-stop-on-dir-change-delay"));
    HM_RETURN_IF_ERROR(copy_required(
        box,
        "resizing-cancel-stop-on-opposite-dir",
        "rink.camera.resizing_cancel_stop_on_opposite_dir",
        ScalarType::kBool,
        box_path + "resizing-cancel-stop-on-opposite-dir"));
    HM_RETURN_IF_ERROR(copy_required(
        box,
        "resizing-stop-cancel-hysteresis-frames",
        "rink.camera.resizing_stop_cancel_hysteresis_frames",
        ScalarType::kInt,
        box_path + "resizing-stop-cancel-hysteresis-frames"));
    HM_RETURN_IF_ERROR(copy_required(
        box,
        "resizing-stop-delay-cooldown-frames",
        "rink.camera.resizing_stop_delay_cooldown_frames",
        ScalarType::kInt,
        box_path + "resizing-stop-delay-cooldown-frames"));
    HM_RETURN_IF_ERROR(copy_required(
        box,
        "resizing-time-to-dest-speed-limit-frames",
        "rink.camera.resizing_time_to_dest_speed_limit_frames",
        ScalarType::kInt,
        box_path + "resizing-time-to-dest-speed-limit-frames"));
    HM_RETURN_IF_ERROR(copy_required(
        box,
        "resizing-time-to-dest-stop-speed-threshold",
        "rink.camera.resizing_time_to_dest_stop_speed_threshold",
        ScalarType::kDouble,
        box_path + "resizing-time-to-dest-stop-speed-threshold"));
  }
  YAML::Node follower = live_boxes[roles.follower_index];
  const YAML::Node follower_name = follower["name"];
  const bool named_follower = follower_name.IsScalar() && follower_name.as<std::string>() == "current_roi_aspect";
  const std::string follower_path =
      named_follower ? "play-tracker.live-boxes[current_roi_aspect]." : "play-tracker.live-boxes[follower].";
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "stop-translation-on-dir-change-delay",
      "rink.camera.stop_on_dir_change_delay",
      ScalarType::kInt,
      follower_path + "stop-translation-on-dir-change-delay"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "cancel-stop-on-opposite-dir",
      "rink.camera.cancel_stop_on_opposite_dir",
      ScalarType::kBool,
      follower_path + "cancel-stop-on-opposite-dir"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "cancel-stop-hysteresis-frames",
      "rink.camera.stop_cancel_hysteresis_frames",
      ScalarType::kInt,
      follower_path + "cancel-stop-hysteresis-frames"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "stop-delay-cooldown-frames",
      "rink.camera.stop_delay_cooldown_frames",
      ScalarType::kInt,
      follower_path + "stop-delay-cooldown-frames"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "post-nonstop-stop-delay-count",
      "rink.camera.breakaway_detection.post_nonstop_stop_delay_count",
      ScalarType::kInt,
      follower_path + "post-nonstop-stop-delay-count"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "sticky-size-ratio-to-frame-width",
      "rink.camera.sticky_size_ratio_to_frame_width",
      ScalarType::kDouble,
      follower_path + "sticky-size-ratio-to-frame-width"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "sticky-translation-gaussian-mult",
      "rink.camera.sticky_translation_gaussian_mult",
      ScalarType::kDouble,
      follower_path + "sticky-translation-gaussian-mult"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "unsticky-translation-size-ratio",
      "rink.camera.unsticky_translation_size_ratio",
      ScalarType::kDouble,
      follower_path + "unsticky-translation-size-ratio"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "scale-dest-width",
      "rink.camera.follower_box_scale_width",
      ScalarType::kDouble,
      follower_path + "scale-dest-width"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "scale-dest-height",
      "rink.camera.follower_box_scale_height",
      ScalarType::kDouble,
      follower_path + "scale-dest-height"));
  std::vector<size_t> normalized_order;
  HM_ASSIGN_OR_RETURN(normalized_order, normalized_playtracker_live_box_order(live_boxes));
  YAML::Node normalized_boxes(YAML::NodeType::Sequence);
  for (const size_t index : normalized_order)
    normalized_boxes.push_back(YAML::Clone(live_boxes[index]));
  play_tracker["live-boxes"] = normalized_boxes;
  return result;
}

} // namespace hm::tracking
