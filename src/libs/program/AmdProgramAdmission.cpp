#include "hstream/src/libs/program/AmdProgramAdmission.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "hstream/src/libs/common/ConfigPath.h"

namespace hm::program {
namespace {
bool Flag(const YAML::Node& node) {
  if (node.IsScalar() && (node.Scalar() == "0" || node.Scalar() == "1"))
    return node.Scalar() == "1";
  return node.as<bool>();
}
int Rank(const config::ProgramConfiguration& config, const std::string& path) {
  const auto found = config.ranks.find(path);
  return found == config.ranks.end() ? 0 : found->second;
}
YAML::Node Choice(
    const config::ProgramConfiguration& config,
    const std::vector<std::string>& paths,
    bool include_null = false) {
  YAML::Node selected;
  int rank = -1;
  for (const auto& path : paths) {
    const auto value = get_node(config.values, path);
    if (value && (include_null || !value->IsNull()) && Rank(config, path) >= rank) {
      selected.reset(YAML::Clone(*value));
      rank = Rank(config, path);
    }
  }
  return selected;
}
} // namespace
absl::Status ValidateAmdProgramAdmission(const config::ProgramConfiguration& config) {
  try {
    const auto stitching = Choice(config, {"stitching.enabled", "pipeline.hmstitcher.enable"});
    if (stitching.IsNull() || !Flag(stitching))
      return absl::UnimplementedError("AMD Program currently requires stitching.enabled=true");
    const auto rotation = Choice(
        config,
        {"game.stitching.stitch-rotate-degrees",
         "game.stitching.stitch_rotate_degrees",
         "game.stitching.post_stitch_rotate_degrees",
         "stitching.stitch-rotate-degrees",
         "stitching.stitch_rotate_degrees",
         "stitching.post_stitch_rotate_degrees",
         "pipeline.hmstitcher.post_stitch_rotate_degrees",
         "pipeline.hmstitcher.post-stitch-rotate-degrees",
         "pipeline.hmstitcher.properties.post-stitch-rotate-degrees",
         "pipeline.hmstitcher.private-properties.post-stitch-rotate-degrees"},
        true);
    // Null means no post-projection rotation, as in the native configurator.
    // The processor independently verifies that the saved output is unrotated.
    const bool cleared_rotation =
        rotation.IsNull() || (rotation.IsScalar() && (rotation.Scalar() == "null" || rotation.Scalar() == "~"));
    if (!cleared_rotation) {
      if (!std::isfinite(rotation.as<double>()))
        return absl::InvalidArgumentError("AMD Program post-stitch rotation must be finite");
      if (rotation.as<double>() != 0)
        return absl::UnimplementedError("AMD Program currently requires unrotated saved output geometry");
    }
    for (const auto* stage : {"primary-gie", "ds-fieldmask", "tracker", "ds-playtracker", "hmplaycropper"}) {
      const auto path = std::string("pipeline.") + stage + ".enable";
      const auto value = get_node(config.values, path);
      if (!value || !Flag(*value))
        return absl::UnimplementedError("AMD Program currently requires " + path + "=1");
    }
    const auto reject = [&](const std::vector<std::string>& paths, const char* feature) {
      const auto value = Choice(config, paths);
      if (!value.IsNull() && Flag(value))
        throw std::runtime_error(
            std::string("AMD Program does not yet support ") + feature + "; disable " + paths.back());
    };
    reject({"pipeline.hmstitcher.configure-only"}, "calibration-only playback");
    // The native generic audio block overrides numbered blocks by layer rank.
    const auto pipeline = config.values["pipeline"];
    for (const auto& entry : pipeline) {
      const auto name = entry.first.as<std::string>();
      if (name.rfind("hmaudio", 0) == 0)
        reject({"pipeline." + name + ".enable", "pipeline.hmaudio.enable"}, "audio");
      if (name.rfind("secondary-gie", 0) == 0)
        reject({"pipeline." + name + ".enable"}, "secondary inference");
      if (name.rfind("dewarper", 0) == 0 || name == "ds-example" || name == "hm-image-meta-merger" ||
          name == "nvds-analytics")
        reject({"pipeline." + name + ".enable"}, name.c_str());
    }
    reject({"pipeline.tracker.reid-enable"}, "appearance reassociation");
    for (const auto* feature : {"pose", "jersey", "action"})
      reject({std::string("pipeline.player-analytics.") + feature + ".enable"}, "player analytics");
    reject({"team.enabled", "pipeline.player-analytics.team.enable"}, "team analytics");
    for (const auto& names :
         {std::pair<const char*, const char*>{"plot_pose", "draw-pose"},
          {"plot_jersey_numbers", "draw-jerseys"},
          {"plot_actions", "draw-actions"},
          {"plot_teams", "draw-teams"}})
      reject(
          {std::string("plot.") + names.first, std::string("pipeline.player-analytics.") + names.second},
          "player overlays");
    const auto reject_drawing = [&](const std::vector<std::string>& canonical, const std::string& native) {
      // Native map_bool_or gives the OR of enabled canonical switches the rank
      // of its highest enabled contributor. Explicit native choices win ties.
      bool enabled = false;
      int selected_rank = -1, disabled_rank = -1;
      for (const auto& path : canonical) {
        const auto value = get_node(config.values, path);
        if (!value || value->IsNull())
          continue;
        const bool flag = Flag(*value);
        if (flag) {
          enabled = true;
          selected_rank = std::max(selected_rank, Rank(config, path));
        } else {
          disabled_rank = std::max(disabled_rank, Rank(config, path));
        }
      }
      if (!enabled)
        selected_rank = disabled_rank;
      const auto dot = native.rfind('.');
      for (const auto* bag : {"", "properties.", "private-properties."}) {
        const auto path = native.substr(0, dot + 1) + bag + native.substr(dot + 1);
        const auto value = get_node(config.values, path);
        if (value && !value->IsNull() && Rank(config, path) >= selected_rank) {
          enabled = Flag(*value);
          selected_rank = Rank(config, path);
        }
      }
      if (enabled)
        throw std::runtime_error("AMD Program does not yet support diagnostic drawing: " + native);
    };
    reject_drawing({"plot.debug_play_tracker", "plot.plot_moving_boxes"}, "pipeline.hmplaycropper.plot-play-tracking");
    reject_drawing({"plot.debug_play_tracker", "plot.plot_moving_boxes"}, "pipeline.ds-playtracker.draw");
    reject_drawing(
        {"plot.debug_play_tracker", "plot.plot_individual_player_tracking"},
        "pipeline.hmplaycropper.plot-player-tracking");
    for (const auto* flag :
         {"plot_boundaries", "plot_all_detections", "plot_trajectories", "plot_speed", "plot_ice_mask"})
      reject({std::string("plot.") + flag}, "diagnostic overlays");
    reject({"pipeline.osd.enable"}, "DeepStream OSD");
    reject({"pipeline.ds-playtracker.private-properties.rink-camera-enable"}, "rink framing");
    for (const auto* property : {"telemetry-db-dir", "telemetry-csv-dir"})
      for (const auto* bag : {"", "properties.", "private-properties."}) {
        const auto path = std::string("pipeline.ds-playtracker.") + bag + property;
        const auto value = get_node(config.values, path);
        if (value && !value->IsNull() && !value->as<std::string>().empty())
          return absl::UnimplementedError("AMD Program does not yet support telemetry recording: " + path);
      }
    const auto interval = get_node(config.values, "pipeline.primary-gie.interval");
    if (interval && interval->as<int>() != 0)
      return absl::UnimplementedError("AMD Program currently requires pipeline.primary-gie.interval=0");
    return absl::OkStatus();
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid AMD feature selection: ") + error.what());
  } catch (const std::exception& error) {
    return absl::UnimplementedError(error.what());
  }
}
} // namespace hm::program
