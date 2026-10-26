#include "hstream/src/libs/tracking/PlayTrackerRuntimeControls.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

#include "hstream/src/libs/common/ConfigPath.h"
#include "hstream/src/libs/common/PlayTrackerConfigRoles.h"

namespace hm::tracking {
namespace {
bool IsBoolean(const std::string& id) {
  return id == "Cancel_Stop_On_Opposite_Direction" || id == "Ignore_Oversized_Players";
}
YAML::Node NativeValue(const CameraSliderSpec& spec, double value) {
  if (std::string(spec.id) == "Oversized_Player_Percent")
    return YAML::Node(value);
  if (IsBoolean(spec.id))
    return YAML::Node(value != 0);
  if (std::string(spec.id) == "Overshoot_Speed_Ratio_x100")
    return YAML::Node(static_cast<double>(std::max(1.0, value)) / spec.divisor);
  if (spec.divisor != 1)
    return YAML::Node(static_cast<double>(value) / spec.divisor);
  return YAML::Node(static_cast<int>(value));
}
} // namespace

absl::StatusOr<CameraControlValues> ReadPlaytrackerControlValues(const YAML::Node& resolved) {
  CameraControlValues result;
  try {
    const auto required = [&](const std::string& path) -> YAML::Node {
      const auto value = get_node(resolved, path);
      if (!value || !value->IsScalar())
        throw std::invalid_argument("Missing required camera control " + path);
      return *value;
    };
    for (const auto& [id, path] : std::map<std::string, std::string>{
             {"Stop_Direction_Change_Delay_Frames", "rink.camera.stop_on_dir_change_delay"},
             {"Stop_Cancel_Hysteresis_Frames", "rink.camera.stop_cancel_hysteresis_frames"},
             {"Stop_Delay_Cooldown_Frames", "rink.camera.stop_delay_cooldown_frames"},
             {"Time_To_Dest_Speed_Limit_Frames", "rink.camera.time_to_dest_speed_limit_frames"},
             {"Ignore_Largest_Count", "rink.tracking.cam_ignore_largest_count"},
             {"Zoom_In_Aggressiveness", "rink.camera.zoom_in_aggressiveness"},
             {"Overshoot_Stop_Delay_Frames", "rink.camera.breakaway_detection.overshoot_stop_delay_count"},
             {"Post_Nonstop_Stop_Delay_Frames", "rink.camera.breakaway_detection.post_nonstop_stop_delay_count"}})
      result[id] = required(path).as<int>();
    result["Cancel_Stop_On_Opposite_Direction"] = required("rink.camera.cancel_stop_on_opposite_dir").as<bool>();
    result["Ignore_Oversized_Players"] = required("rink.tracking.cam_ignore_oversized").as<bool>();
    if (!required("rink.tracking.cam_ignore_largest").as<bool>())
      result["Ignore_Largest_Count"] = 0;
    for (const auto& [id, path, scale] :
         {std::tuple{"Oversized_Player_Percent", "rink.tracking.cam_oversized_percent", 1.0},
          std::tuple{
              "Overshoot_Speed_Ratio_x100", "rink.camera.breakaway_detection.overshoot_scale_speed_ratio", 100.0}}) {
      const double number = required(path).as<double>() * scale;
      if (!std::isfinite(number) || number < 0 || (scale != 1.0 && number > std::numeric_limits<int>::max()))
        throw std::invalid_argument(std::string("Invalid camera control ") + path);
      result[id] = scale == 1.0 ? number : std::llround(number);
    }
    for (const char* id :
         {"Apply_To_Fast_Box",
          "Apply_To_Follower_Box",
          "Max_Speed_X_x10",
          "Max_Speed_Y_x10",
          "Max_Accel_X_x10",
          "Max_Accel_Y_x10"})
      result[id] = required(std::string("hstream_ui.camera_controls.") + id).as<int>();
    for (auto& [id, value] : result) {
      const auto override = get_node(resolved, "hstream_ui.camera_controls." + id);
      if (override)
        value = override->as<double>();
    }
    const auto specs = PlaytrackerControlSpecs(result);
    if (!specs.ok())
      return specs.status();
    return result;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid resolved camera controls: ") + error.what());
  }
}

bool ValidPlaytrackerControlValue(const CameraSliderSpec& spec, double value) {
  if (!std::isfinite(value) || value < spec.minimum)
    return false;
  // The desktop percentage field is QDoubleSpinBox with the full finite
  // double range, unlike the integer sliders used in experiment metadata.
  if (std::string(spec.id) == "Oversized_Player_Percent")
    return true;
  return value <= spec.maximum && std::floor(value) == value;
}

absl::StatusOr<std::vector<CameraSliderSpec>> PlaytrackerControlSpecs(const CameraControlValues& defaults) {
  try {
    const auto lookup = [&](const char* id) {
      const double value = defaults.at(id);
      if (!std::isfinite(value))
        throw std::invalid_argument("Nonfinite baseline camera control");
      return static_cast<int>(std::clamp(
          value,
          static_cast<double>(std::numeric_limits<int>::min()),
          static_cast<double>(std::numeric_limits<int>::max())));
    };
    auto specs = tracking_control_specs(lookup);
    for (const auto& group : {motion_control_specs(lookup), player_control_specs(lookup)})
      specs.insert(specs.end(), group.begin(), group.end());
    for (const auto& spec : specs)
      if (!ValidPlaytrackerControlValue(spec, defaults.at(spec.id)))
        return absl::InvalidArgumentError(std::string("Invalid baseline camera control ") + spec.id);
    return specs;
  } catch (const std::exception&) {
    return absl::InvalidArgumentError("Missing or invalid required baseline camera control");
  }
}

absl::StatusOr<YAML::Node> BuildPlaytrackerRuntimeControls(
    const YAML::Node& base,
    const CameraControlValues& values,
    const CameraControlValues& defaults,
    const std::set<std::string>& changed,
    bool force_all_targets) {
  const auto specs = PlaytrackerControlSpecs(defaults);
  if (!specs.ok())
    return specs.status();
  std::map<std::string, CameraSliderSpec> by_id;
  for (const auto& spec : *specs) {
    by_id.emplace(spec.id, spec);
    const auto found = values.find(spec.id);
    if (found == values.end() || !ValidPlaytrackerControlValue(spec, found->second))
      return absl::InvalidArgumentError(std::string("Missing or invalid camera control ") + spec.id);
  }
  for (const auto& id : changed)
    if (!by_id.count(id))
      return absl::InvalidArgumentError("Unknown live camera control " + id);
  if (!base.IsMap())
    return absl::InvalidArgumentError("Native playtracker config must be a map");
  try {
    YAML::Node result = YAML::Clone(base);
    if (!result["play-tracker"].IsMap())
      result["play-tracker"] = YAML::Node(YAML::NodeType::Map);
    auto tracker = result["play-tracker"];
    if (!tracker["live-boxes"].IsSequence())
      tracker["live-boxes"] = YAML::Node(YAML::NodeType::Sequence);
    auto boxes = tracker["live-boxes"];
    if (boxes.size() == 0) {
      for (const char* name : {"current_roi", "current_roi_aspect"}) {
        YAML::Node box(YAML::NodeType::Map);
        box["name"] = name;
        boxes.push_back(box);
      }
    }
    const auto roles = resolve_playtracker_live_box_roles(boxes);
    if (!roles.ok())
      return roles.status();
    tracker["hstream-apply-to-fast-box"] = force_all_targets || values.at("Apply_To_Fast_Box") != 0;
    tracker["hstream-apply-to-follower-box"] = force_all_targets || values.at("Apply_To_Follower_Box") != 0;
    tracker["hstream-runtime-tuning"] = YAML::Node(YAML::NodeType::Map);
    auto tuning = tracker["hstream-runtime-tuning"];
    for (const auto& id : changed) {
      const auto& spec = by_id.at(id);
      if (spec.runtime_key)
        tuning[spec.runtime_key] = NativeValue(spec, values.at(id));
    }
    for (const char* id : {"Overshoot_Stop_Delay_Frames", "Overshoot_Speed_Ratio_x100"}) {
      const auto& spec = by_id.at(id);
      if (values.at(id) != defaults.at(id))
        tracker[spec.runtime_key] = NativeValue(spec, values.at(id));
    }
    auto apply_box = [&](size_t index) {
      for (const char* id :
           {"Stop_Direction_Change_Delay_Frames",
            "Cancel_Stop_On_Opposite_Direction",
            "Stop_Cancel_Hysteresis_Frames",
            "Stop_Delay_Cooldown_Frames",
            "Time_To_Dest_Speed_Limit_Frames",
            "Post_Nonstop_Stop_Delay_Frames",
            "Max_Speed_X_x10",
            "Max_Speed_Y_x10",
            "Max_Accel_X_x10",
            "Max_Accel_Y_x10"}) {
        const auto& spec = by_id.at(id);
        if (values.at(id) != defaults.at(id))
          boxes[index][spec.runtime_key] = NativeValue(spec, values.at(id));
      }
    };
    if (values.at("Apply_To_Fast_Box") != 0)
      apply_box(roles->fast_index);
    if (values.at("Apply_To_Follower_Box") != 0)
      apply_box(roles->follower_index);
    return result;
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid native playtracker config: ") + error.what());
  }
}
} // namespace hm::tracking
