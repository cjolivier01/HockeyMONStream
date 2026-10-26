#include "hstream/src/libs/stitching/StitchingCalibrationControls.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <stdexcept>

#include "hstream/src/libs/common/StatusMacros.h"
#include "hstream/src/libs/stitching/PlayerFrameSelection.h"
#include "hstream/src/libs/stitching/RinkMaskFrameTime.h"
#include "hstream/src/libs/stitching/StitchingReframe.h"
#include "hstream/src/libs/stitching/TransactionState.h"

namespace hm::stitching {
namespace {
YAML::Node at(const YAML::Node& root, std::initializer_list<const char*> path) {
  YAML::Node result = root;
  for (const char* key : path) {
    if (!result.IsDefined() || !result.IsMap())
      return YAML::Node(YAML::NodeType::Undefined);
    const auto child = static_cast<const YAML::Node&>(result)[key];
    if (!child.IsDefined())
      return YAML::Node(YAML::NodeType::Undefined);
    result.reset(child);
  }
  return result;
}

std::optional<size_t> stage_index(const std::string& stage) {
  static constexpr std::array<const char*, 8> stages{
      "input", "orientation", "features", "matching", "optimizer", "leveling", "canvas", "rink-mask"};
  const auto found = std::find(stages.begin(), stages.end(), stage);
  return found == stages.end() ? std::nullopt : std::optional<size_t>(found - stages.begin());
}

std::string scalar(const YAML::Node& root, std::initializer_list<const char*> path) {
  const auto value = at(root, path);
  return value.IsScalar() ? value.as<std::string>() : std::string();
}

YAML::Node cleanup_guard(const YAML::Node& source, StitchingCalibrationCleanup cleanup) {
  auto config = YAML::Clone(source);
  const auto remove = [&](std::initializer_list<const char*> path, const char* key) {
    auto parent = at(config, path);
    if (parent.IsMap())
      parent.remove(key);
  };
  // Only these cache fields can change inside native cleanup. All controls,
  // source/selection references and the complete pending state stay guarded.
  for (const auto& path : {std::initializer_list<const char*>{"stitching"}, {"game", "stitching"}}) {
    remove(path, "control_points");
    if (cleanup == StitchingCalibrationCleanup::kAll)
      remove(path, "frame_offsets");
  }
  remove({"pipeline", "hmplaycropper"}, "scoreboard-perspective-polygon");
  remove({"rink", "scoreboard"}, "perspective_polygon");
  for (const char* key :
       {"ice_contours_mask_count",
        "ice_contours_mask_centroid",
        "ice_contours_combined_bbox",
        "stitched_output_generation",
        "stitched_output_persisted_rotation_degrees",
        "stitched_output_pending_generation",
        "stitched_output_pending_authorization_id",
        "stitched_output_pending_owner_process",
        "stitched_output_pending_previous_generation",
        "stitched_output_pending_previous_authorization_id",
        "stitched_output_pending_previous_owner_process",
        "stitched_output_pending_completed_scoreboard_polygon",
        "stitched_output_pending_scoreboard_polygon_invalidated"})
    remove({"rink"}, key);
  return config;
}

YAML::Node canonical_guard(const YAML::Node& node, size_t depth = 0) {
  if (depth > 64)
    throw std::invalid_argument("Calibration configuration is too deeply nested");
  if (node.IsMap()) {
    std::map<std::string, YAML::Node> entries;
    for (const auto& entry : node) {
      auto value = canonical_guard(entry.second, depth + 1);
      // Cleanup prunes empty parents; they carry no control or selection data.
      if (!value.IsMap() || value.size())
        entries.emplace(entry.first.as<std::string>(), std::move(value));
    }
    YAML::Node result(YAML::NodeType::Map);
    for (const auto& entry : entries)
      result[entry.first] = entry.second;
    return result;
  }
  if (node.IsSequence()) {
    YAML::Node result(YAML::NodeType::Sequence);
    for (const auto& entry : node)
      result.push_back(canonical_guard(entry, depth + 1));
    return result;
  }
  return YAML::Clone(node);
}

void release_inputs(YAML::Node& config, bool selected, bool manual, bool sources) {
  if (selected) {
    config["stitching"].remove("calibration_frame_selection");
    config["stitching"].remove("calibration_frame_inputs_fingerprint");
  }
  if (manual) {
    config["stitching"].remove("manual_control_points");
    config["hstream_ui"]["stitching_calibration"].remove("match_snapshot");
  }
  if (sources) {
    // Stored bundles are immutable historical evidence. Release references;
    // never rewrite manifests or delete their source directories.
    InvalidateStitchingSourceInputs(config);
  }
}
} // namespace

int RemoveManualStitchingCleanConfigKeys(YAML::Node& config) {
  const auto remove = [&](std::initializer_list<const char*> path) {
    auto parent = config;
    for (auto key = path.begin(); key != path.end() - 1; ++key) {
      if (!parent.IsMap())
        return false;
      const auto child = static_cast<const YAML::Node&>(parent)[*key];
      if (!child.IsDefined())
        return false;
      parent.reset(child);
    }
    return parent.IsMap() && parent.remove(*(path.end() - 1));
  };
  int removed = 0;
  // These are results of the previous calibration, including its crop-review
  // acknowledgement. Leave the selected projection, camera, sampling and rink
  // profile intact, but require new leveling/crop decisions after a manual clean.
  removed += remove({"hstream_ui", "projection_crop_geometry"}) ? 1 : 0;
  for (const char* key : {"crop", "auto_crop", "rotation_degrees"}) {
    removed += remove({"stitching", "projection_framing", key}) ? 1 : 0;
    // A generated backend override can restore its previous private framing
    // during reload; neither copy may bring the cleaned calibration back.
    if (remove({"hstream_ui", "generated_stitching_backend_choices", "projection_framing", key}))
      ++removed;
    if (remove({"hstream_ui", "generated_stitching_backend_choices", "previous_projection_framing", key}))
      ++removed;
  }
  const auto selected_frames = at(config, {"stitching", "calibration_frame_selection"});
  if (!selected_frames.IsDefined() || selected_frames.IsNull()) {
    removed += remove({"stitching", "frame_offsets"}) ? 1 : 0;
    removed += remove({"game", "stitching", "frame_offsets"}) ? 1 : 0;
  }
  removed += remove({"stitching", "control_points"}) ? 1 : 0;
  removed += remove({"game", "stitching", "control_points"}) ? 1 : 0;
  removed += remove({"stitching", "generated_field_mask_post_stitch_rotate_degrees"}) ? 1 : 0;
  removed += remove({"rink", "stitched_output_generation"}) ? 1 : 0;
  removed += remove({"rink", "stitched_output_persisted_rotation_degrees"}) ? 1 : 0;
  removed += remove({"rink", "stitched_output_pending_generation"}) ? 1 : 0;
  removed += remove({"rink", "stitched_output_pending_authorization_id"}) ? 1 : 0;
  removed += remove({"rink", "stitched_output_pending_owner_process"}) ? 1 : 0;
  removed += remove({"rink", "stitched_output_pending_previous_generation"}) ? 1 : 0;
  removed += remove({"rink", "stitched_output_pending_previous_authorization_id"}) ? 1 : 0;
  removed += remove({"rink", "stitched_output_pending_previous_owner_process"}) ? 1 : 0;
  remove({"rink", "stitched_output_pending_completed_scoreboard_polygon"});
  remove({"rink", "stitched_output_pending_scoreboard_polygon_invalidated"});
  remove({"pipeline", "hmplaycropper", "scoreboard-perspective-polygon"});
  // Shadow inherited polygons instead of reviving them on the next merge.
  config["rink"]["scoreboard"]["perspective_polygon"] = YAML::Node(YAML::NodeType::Null);
  ++removed;
  removed += remove({"rink", "ice_contours_mask_count"}) ? 1 : 0;
  removed += remove({"rink", "ice_contours_mask_centroid"}) ? 1 : 0;
  removed += remove({"rink", "ice_contours_combined_bbox"}) ? 1 : 0;
  removed += remove({"hstream_ui", "stitching_calibration", "status"}) ? 1 : 0;
  removed += remove({"hstream_ui", "stitching_calibration", "rink_mask_status"}) ? 1 : 0;
  removed += remove({"hstream_ui", "stitching_calibration", "stale_from"}) ? 1 : 0;
  removed += remove({"hstream_ui", "stitching_calibration", "artifacts_invalidated"}) ? 1 : 0;
  removed += remove({"hstream_ui", "stitching_calibration", "invalidation_id"}) ? 1 : 0;
  removed += remove({"hstream_ui", "stitching_calibration", "backend_generation"}) ? 1 : 0;
  removed += remove({"hstream_ui", "stitching_calibration", "reframe"}) ? 1 : 0;
  return removed;
}

bool InvalidateStitchingSourceInputs(YAML::Node& config) {
  bool changed = false;
  const auto remove = [&](std::initializer_list<const char*> path, const char* key) {
    auto parent = at(config, path);
    if (parent.IsMap())
      changed = parent.remove(key) || changed;
  };
  for (const char* key :
       {"frame_offsets",
        "manual_control_points",
        "calibration_frame_selection",
        "calibration_frame_inputs_fingerprint"})
    remove({"stitching"}, key);
  remove({"game", "stitching"}, "frame_offsets");
  remove({"hstream_ui", "stitching_calibration"}, "match_snapshot");
  remove({"hstream_ui", "stitching_calibration"}, "reframe");
  remove({"rink", "mask_frame"}, "input_id");
  return changed;
}

bool StitchingCalibrationChanges::geometry() const {
  return force || sources || frame_time || sync_method || control_points || frame_count || matcher || backend ||
      camera || projection || parameters || framing || autooptimizer || canvas_required;
}

const char* StitchingCalibrationCleanupName(StitchingCalibrationCleanup cleanup) {
  switch (cleanup) {
    case StitchingCalibrationCleanup::kNone:
      return "none";
    case StitchingCalibrationCleanup::kAll:
      return "all";
    case StitchingCalibrationCleanup::kControlPoints:
      return "control-points";
  }
  return "invalid";
}

absl::StatusOr<int> ResolveStitchingCalibrationMaxWidth(const YAML::Node& private_config, const YAML::Node& defaults) {
  try {
    for (const auto& config : {private_config, defaults}) {
      auto width = at(config, {"stitching", "max_output_width"});
      if (!width.IsDefined()) {
        for (const char* section : {"properties", "private-properties"}) {
          for (const char* key :
               {"max-output-width", "max_output_width", "stitch-max-output-width", "stitch_max_output_width"}) {
            width.reset(at(config, {"pipeline", "hmstitcher", section, key}));
            if (width.IsDefined())
              break;
          }
          if (width.IsDefined())
            break;
        }
      }
      if (!width.IsDefined())
        continue;
      if (width.IsNull())
        return 0;
      const int value = width.as<int>();
      if (value < 0)
        return absl::InvalidArgumentError("Stitching canvas width must be nonnegative or null");
      return value;
    }
    return absl::InvalidArgumentError("Missing baseline stitching canvas width");
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Invalid stitching canvas width: " + std::string(error.what()));
  }
}

absl::StatusOr<StitchingCalibrationControls> ReadStitchingCalibrationControls(const YAML::Node& effective) {
  try {
    StitchingCalibrationControls result;
    HM_ASSIGN_OR_RETURN(result.control_points, read_control_point_limit(effective));
    result.frame_count = at(effective, {"stitching", "calibration_frame_count"}).as<int>();
    const auto width = at(effective, {"stitching", "max_output_width"});
    result.max_output_width = width.IsNull() ? 0 : width.as<int>();
    if (result.frame_count < 1 || result.frame_count > kPlayerFrameMaximumPairs || result.max_output_width < 0)
      return absl::InvalidArgumentError("Invalid resolved calibration frame count or canvas width");
    std::optional<int64_t> time;
    HM_ASSIGN_OR_RETURN(
        time, ParseRinkMaskFrameTime(at(effective, {"stitching", "stitch_frame_time"}).as<std::string>()));
    if (!time || *time < 0 || *time >= 24LL * 3600 * kPlayerFrameSecond)
      return absl::InvalidArgumentError("Calibration reference time must be within one day");
    result.frame_time = FormatRinkMaskFrameTime(time);
    result.sync_method = at(effective, {"stitching", "sync_method"}).as<std::string>();
    if (result.sync_method != "audio" && result.sync_method != "imu" && result.sync_method != "auto")
      return absl::InvalidArgumentError("stitching.sync_method must be audio, imu, or auto");
    ControlPointMatcher matcher;
    MappingBackend backend;
    StitchProjection projection;
    HM_ASSIGN_OR_RETURN(matcher, resolve_control_point_matcher(effective));
    HM_ASSIGN_OR_RETURN(
        backend, ParseMappingBackend(at(effective, {"stitching", "mapping_backend"}).as<std::string>()));
    HM_ASSIGN_OR_RETURN(
        projection, ParseStitchProjection(at(effective, {"stitching", "projection"}).as<std::string>()));
    HM_RETURN_IF_ERROR(ValidateMappingBackendProjection(backend, projection));
    result.matcher = ControlPointMatcherName(matcher);
    result.backend = MappingBackendName(backend);
    result.projection = StitchProjectionName(projection);
    result.autooptimizer = at(effective, {"stitching", "run_autooptimizer"}).as<bool>();
    ControlPointResolution resolution;
    HM_ASSIGN_OR_RETURN(resolution, read_control_point_resolution(effective));
    result.resolution = ControlPointResolutionName(resolution);
    hm::onnx::ExecutionProvider provider;
    HM_ASSIGN_OR_RETURN(provider, read_control_point_execution_provider(effective));
    result.execution_provider = hm::onnx::ExecutionProviderName(provider);
    HM_ASSIGN_OR_RETURN(result.filtering, read_control_point_filtering(effective));
    HM_ASSIGN_OR_RETURN(result.camera, read_stitch_camera_selection(effective));
    HM_ASSIGN_OR_RETURN(result.projection_parameters, read_stitch_projection_parameters(effective, projection));
    HM_ASSIGN_OR_RETURN(result.framing, read_stitch_projection_framing(effective));
    if (backend == MappingBackend::kNona)
      HM_RETURN_IF_ERROR(ValidateStitchProjectionFraming(projection, result.projection_parameters, result.framing));
    return result;
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Invalid resolved calibration controls: " + std::string(error.what()));
  }
}

absl::StatusOr<StitchingCalibrationState> ReadStitchingCalibrationState(
    const YAML::Node& private_config,
    const YAML::Node& effective) {
  try {
    StitchingCalibrationState result;
    HM_ASSIGN_OR_RETURN(result.controls, ReadStitchingCalibrationControls(effective));
    const auto state = at(private_config, {"hstream_ui", "stitching_calibration"});
    const auto points = at(state, {"control_points"});
    result.control_points_present = points.IsScalar();
    if (result.control_points_present)
      result.controls.control_points = points.as<int>();
    std::string selection;
    HM_ASSIGN_OR_RETURN(selection, player_frame_selection_fingerprint(private_config));
    if (!selection.empty())
      result.selected_frame_count = at(private_config, {"stitching", "calibration_frame_selection", "selected"}).size();
    auto count = at(state, {"frame_count"});
    if (!count.IsScalar())
      count.reset(at(private_config, {"stitching", "calibration_frame_count"}));
    result.frame_count_present = count.IsScalar() || result.selected_frame_count.has_value();
    if (count.IsScalar())
      result.controls.frame_count = count.as<int>();
    else if (result.selected_frame_count)
      result.controls.frame_count = *result.selected_frame_count;
    result.status = scalar(state, {"status"});
    result.stale_from = scalar(state, {"stale_from"});
    const auto invalidated = at(state, {"artifacts_invalidated"});
    result.artifacts_invalidated = invalidated.IsScalar() && invalidated.as<bool>();
    return result;
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Invalid saved calibration state: " + std::string(error.what()));
  }
}

absl::StatusOr<bool> ApplyStitchingCalibrationInputEdits(
    const StitchingCalibrationState& before,
    const StitchingCalibrationControls& desired,
    bool force,
    bool sources_changed,
    YAML::Node& config) {
  if (desired.control_points < 1 || desired.frame_count < 1 || desired.frame_count > kPlayerFrameMaximumPairs)
    return absl::InvalidArgumentError("Calibration inputs require positive bounded controls");
  const auto& saved = before.controls;
  const bool retain_selected =
      before.selected_frame_count.has_value() && saved.frame_count == desired.frame_count && !sources_changed;
  const bool time_changed = !before.frame_time_valid || saved.frame_time != desired.frame_time;
  if (time_changed && retain_selected)
    return absl::FailedPreconditionError(
        "Cannot change the reference time while keeping the selected calibration frames. Restore the saved "
        "reference time, or explicitly change the frame count to select a new frame set.");
  const bool matcher_changed = saved.matcher != desired.matcher || saved.resolution != desired.resolution ||
      saved.execution_provider != desired.execution_provider || saved.filtering != desired.filtering;
  const bool release_manual = force || sources_changed || time_changed || saved.sync_method != desired.sync_method ||
      !before.control_points_present || saved.control_points != desired.control_points || !before.frame_count_present ||
      saved.frame_count != desired.frame_count || matcher_changed || saved.camera != desired.camera;
  release_inputs(config, saved.frame_count != desired.frame_count || sources_changed, release_manual, sources_changed);
  config["stitching"]["calibration_frame_count"] = desired.frame_count;
  config["hstream_ui"]["stitching_calibration"]["control_points"] = desired.control_points;
  config["hstream_ui"]["stitching_calibration"]["frame_count"] = desired.frame_count;
  return retain_selected;
}

absl::StatusOr<StitchingCalibrationPlan> PrepareStitchingCalibrationControlsLocked(
    const std::filesystem::path& game,
    const StitchingCalibrationState& before,
    const StitchingCalibrationControls& desired,
    const YAML::Node& before_effective,
    const YAML::Node& desired_effective,
    const std::string& owner,
    bool force,
    bool sources_changed,
    bool canvas_required,
    bool canvas_cleanup,
    YAML::Node& desired_private) {
  if (owner.empty() || owner.size() > 256 || desired.control_points < 1 || desired.frame_count < 1 ||
      desired.frame_count > kPlayerFrameMaximumPairs)
    return absl::InvalidArgumentError("Calibration staging requires an owner and positive bounded controls");
  try {
    StitchingCalibrationPlan plan;
    plan.owner = owner;
    auto& c = plan.changes;
    const auto& saved = before.controls;
    c.control_points = !before.control_points_present || saved.control_points != desired.control_points;
    c.frame_count = !before.frame_count_present || saved.frame_count != desired.frame_count;
    c.frame_time = !before.frame_time_valid || saved.frame_time != desired.frame_time;
    c.sync_method = saved.sync_method != desired.sync_method;
    c.matcher = saved.matcher != desired.matcher || saved.resolution != desired.resolution ||
        saved.execution_provider != desired.execution_provider || saved.filtering != desired.filtering;
    c.backend = saved.backend != desired.backend;
    c.camera = saved.camera != desired.camera;
    c.autooptimizer =
        (saved.backend == "nona" && saved.autooptimizer) != (desired.backend == "nona" && desired.autooptimizer);
    c.projection = saved.projection != desired.projection;
    c.parameters = saved.projection_parameters != desired.projection_parameters;
    c.framing = (saved.backend == "nona" || desired.backend == "nona") && saved.framing != desired.framing;
    c.width = saved.max_output_width != desired.max_output_width;
    c.canvas_required = canvas_required;
    c.canvas_cleanup = canvas_cleanup;
    c.force = force;
    c.sources = sources_changed;
    YAML::Node staged = YAML::Clone(desired_private);
    YAML::Node effective = YAML::Clone(desired_effective);
    const bool release_selection = saved.frame_count != desired.frame_count || sources_changed;
    for (auto* config : {&staged, &effective}) {
      HM_ASSIGN_OR_RETURN(
          plan.retain_selected_frames,
          ApplyStitchingCalibrationInputEdits(before, desired, force, sources_changed, *config));
    }
    // Synchronization and reference-time edits change alignment inputs even
    // when a simultaneous view edit would otherwise qualify for reframe.
    if (force || release_selection || c.sync_method || c.frame_time) {
      ClearStitchingReframeIntent(staged);
    } else {
      HM_ASSIGN_OR_RETURN(
          plan.alignment_reused,
          PrepareResolvedStitchingReframeIntentLocked(game, before_effective, effective, owner, staged));
    }
    if (c.geometry()) {
      staged["rink"]["scoreboard"]["perspective_polygon"] = YAML::Node(YAML::NodeType::Null);
      staged["pipeline"]["hmplaycropper"].remove("scoreboard-perspective-polygon");
    }
    plan.retain_manual_matches = at(staged, {"stitching", "manual_control_points"}).IsScalar();
    plan.required = c.geometry() || before.status != "complete" || plan.alignment_reused;
    auto calibration = staged["hstream_ui"]["stitching_calibration"];
    calibration["invalidation_id"] = owner;
    if (!plan.required) {
      desired_private = std::move(staged);
      return plan;
    }
    const bool canvas_changed =
        c.backend || c.camera || c.projection || c.parameters || c.framing || c.autooptimizer || c.canvas_required;
    std::string stale = before.stale_from;
    if (!stage_index(stale))
      stale = canvas_changed && !c.matcher && !c.control_points && !c.frame_count ? "canvas" : "input";
    if ((c.control_points || c.matcher) && before.status == "complete")
      stale = "features";
    else if (
        (c.control_points || c.matcher) && before.control_points_present &&
        *stage_index("features") < *stage_index(stale))
      stale = "features";
    if (canvas_changed && !c.matcher && !c.control_points && !c.frame_count &&
        *stage_index("canvas") < *stage_index(stale))
      stale = "canvas";
    if (force || sources_changed || c.frame_time || c.sync_method || c.frame_count)
      stale = "input";
    bool from_points = !force && !sources_changed && !c.frame_time && stale == "features" &&
        (c.control_points || c.matcher || !before.artifacts_invalidated);
    bool all = force || sources_changed || c.frame_time || c.sync_method ||
        (stale != "features" &&
         (!before.artifacts_invalidated || c.control_points || c.matcher || c.backend || c.projection || c.parameters ||
          c.autooptimizer || c.camera || c.framing || c.canvas_cleanup));
    const bool width_only = before.status == "complete" && c.width && !force && !sources_changed && !c.frame_time &&
        !c.sync_method && !c.frame_count && !c.matcher && !c.backend && !c.projection && !c.camera && !c.parameters &&
        !c.framing && !c.autooptimizer;
    if (width_only && !c.canvas_cleanup)
      all = false;
    if ((plan.retain_selected_frames || plan.retain_manual_matches) && all) {
      all = false;
      from_points = true;
    }
    if (plan.alignment_reused) {
      all = false;
      from_points = false;
      stale = "canvas";
    }
    plan.start_stage = stale;
    plan.cleanup = all ? StitchingCalibrationCleanup::kAll
        : from_points  ? StitchingCalibrationCleanup::kControlPoints
                       : StitchingCalibrationCleanup::kNone;
    calibration["status"] = "pending";
    calibration["rink_mask_status"] = "pending";
    calibration["stale_from"] = stale;
    calibration["artifacts_invalidated"] = !(all || from_points);
    desired_private = std::move(staged);
    return plan;
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Cannot prepare calibration controls: " + std::string(error.what()));
  }
}

absl::Status CompleteStitchingCalibrationCleanup(
    const std::filesystem::path& game,
    const StitchingCalibrationPlan& plan,
    const YAML::Node& prepared_config) {
  if (plan.cleanup == StitchingCalibrationCleanup::kNone)
    return absl::OkStatus();
  auto lock = GameConfigTransactionLock::Acquire(game);
  if (!lock.ok())
    return lock.status();
  try {
    std::string bytes;
    HM_ASSIGN_OR_RETURN(
        bytes, read_bounded_regular_file_no_follow(game / "config.yaml", 4 * 1024 * 1024, "calibration config"));
    auto current = YAML::Load(bytes);
    auto state = current["hstream_ui"]["stitching_calibration"];
    if (scalar(state, {"invalidation_id"}) != plan.owner || scalar(state, {"status"}) != "pending" ||
        scalar(state, {"stale_from"}) != plan.start_stage ||
        YAML::Dump(canonical_guard(cleanup_guard(current, plan.cleanup))) !=
            YAML::Dump(canonical_guard(cleanup_guard(prepared_config, plan.cleanup))))
      return absl::AbortedError("Calibration cleanup was superseded by a newer dependency invalidation");
    state["artifacts_invalidated"] = true;
    return publish_game_config(game, YAML::Dump(current) + "\n");
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("Cannot complete calibration cleanup: " + std::string(error.what()));
  }
}
} // namespace hm::stitching
