#include "hstream/src/libs/stitching/StitchingCalibrationControls.h"

#include <unistd.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/stitching/ConfigureStitchingCore.h"
#include "hstream/src/libs/stitching/PlayerFrameSelection.h"

namespace {
namespace st = hm::stitching;
namespace fs = std::filesystem;
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T>
T take(absl::StatusOr<T> value) {
  if (!value.ok())
    throw std::runtime_error(value.status().ToString());
  return std::move(*value);
}
void check(const absl::Status& status) {
  if (!status.ok())
    throw std::runtime_error(status.ToString());
}
void write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << text;
}

YAML::Node selected_plan(int count) {
  st::PlayerFrameSelectionPlan plan;
  plan.settings.frame_count = count;
  plan.sources = {{"/selected/left.mp4", 1, 0}, {"/selected/right.mp4", 1, 0}};
  for (const char* key :
       {"source_context",
        "baseline_generation",
        "output_generation",
        "detector_identity",
        "rink_mask_sha256",
        "rink_mask_revision",
        "fieldmask_settings",
        "output_rotation_degrees"})
    plan.context[key] = "retained";
  for (int i = 0; i < count; ++i) {
    st::PlayerFrameObservation frame;
    frame.pair.timeline_pts_ns = i * st::kPlayerFrameSecond;
    for (int camera = 0; camera < 2; ++camera)
      frame.pair.cameras[camera] = {plan.sources[camera].path, frame.pair.timeline_pts_ns};
    frame.coverage = {0};
    frame.eligible_people = 1;
    frame.size_band_counts = {1, 0, 0};
    frame.quality = 1;
    plan.selected.push_back(frame);
  }
  plan.fingerprint = take(st::PlayerFrameSelectionFingerprint(plan));
  return st::PlayerFrameSelectionPlanYaml(plan);
}

YAML::Node baseline(bool selected = false, bool manual = false) {
  auto result = YAML::Clone(take(hm::baseline_config::load()).values);
  result["stitching"]["camera_config"] = "manual";
  result["stitching"]["camera_automation"]["fov"] = false;
  result["stitching"]["camera_automation"]["lens"] = false;
  result["stitching"]["control_point_matcher"] = "akaze";
  result["stitching"]["calibration_frame_count"] = 2;
  auto state = result["hstream_ui"]["stitching_calibration"];
  state["status"] = "complete";
  state["invalidation_id"] = "source-owner";
  state["frame_count"] = 2;
  state["control_points"] = 1500;
  if (selected) {
    result["stitching"]["calibration_frame_selection"] = selected_plan(2);
    result["stitching"]["calibration_frame_inputs_fingerprint"] =
        result["stitching"]["calibration_frame_selection"]["fingerprint"].as<std::string>();
  }
  if (manual) {
    result["stitching"]["manual_control_points"] = std::string(64, 'a');
    state["match_snapshot"] = "manual-evidence";
  }
  result["stitching"]["frame_offsets"]["left"] = 7;
  result["rink"]["mask_frame"]["input_id"] = std::string(64, 'b');
  return result;
}

st::StitchingCalibrationPlan prepare(
    const fs::path& game,
    const YAML::Node& before,
    YAML::Node& desired,
    bool sources = false) {
  return take(
      st::PrepareStitchingCalibrationControlsLocked(
          game,
          take(st::ReadStitchingCalibrationState(before, before)),
          take(st::ReadStitchingCalibrationControls(desired)),
          before,
          desired,
          "new-owner",
          false,
          sources,
          false,
          false,
          desired));
}

void matrix(const fs::path& game) {
  for (const char* document : {"", "{}", "{stitching: {}}", "{hstream_ui: {}}", "{rink: {mask_frame: {}}}"}) {
    auto sparse = YAML::Load(document);
    const auto original = YAML::Dump(sparse);
    require(
        !st::InvalidateStitchingSourceInputs(sparse) && YAML::Dump(sparse) == original,
        "Missing optional source keys must be safe and leave sparse configs unchanged");
    const auto saved = take(st::ReadStitchingCalibrationState(sparse, baseline()));
    require(
        !saved.control_points_present && !saved.frame_count_present && saved.status.empty(),
        "Missing saved calibration bookkeeping must remain absent without invalid YAML nodes");
  }
  {
    auto defaults = baseline();
    YAML::Node private_config;
    private_config["pipeline"]["hmstitcher"]["properties"]["max-output-width"] = 1280;
    require(
        take(st::ResolveStitchingCalibrationMaxWidth(private_config, defaults)) == 1280,
        "Private native width alias must override the canonical baseline");
    private_config["stitching"]["max_output_width"] = YAML::Node(YAML::NodeType::Null);
    require(
        take(st::ResolveStitchingCalibrationMaxWidth(private_config, defaults)) == 0,
        "Explicit canonical Auto must override legacy width aliases");
  }
  {
    auto before = baseline(), desired = YAML::Clone(before);
    desired["stitching"]["sync_method"] = "imu";
    desired["stitching"]["projection_framing"]["crop"] = std::vector<double>{0.1, 0.9, 0.1, 0.9};
    const auto plan = prepare(game, before, desired);
    require(
        plan.required && !plan.alignment_reused && plan.start_stage == "input" &&
            plan.cleanup == st::StitchingCalibrationCleanup::kAll,
        "Synchronization plus view changes must solve new inputs instead of reusing stale alignment");
  }
  {
    auto before = baseline(), desired = YAML::Clone(before);
    const auto plan = prepare(game, before, desired);
    require(
        !plan.required && plan.cleanup == st::StitchingCalibrationCleanup::kNone &&
            desired["hstream_ui"]["stitching_calibration"]["status"].as<std::string>() == "complete" &&
            desired["hstream_ui"]["stitching_calibration"]["invalidation_id"].as<std::string>() == "new-owner",
        "Unchanged Play must reserve an owner without requesting calibration");
  }
  for (const auto& name : {"budget", "matcher", "provider", "camera", "backend", "count", "sync", "time", "sources"}) {
    auto before = baseline(true, true), desired = YAML::Clone(before);
    const std::string kind(name);
    if (kind == "budget")
      desired["hstream_ui"]["stitching_calibration"]["control_points"] = 1600;
    if (kind == "matcher")
      desired["stitching"]["control_point_matcher"] = "superpoint-lightglue";
    if (kind == "provider")
      desired["stitching"]["control_point_execution_provider"] = "cpu";
    if (kind == "camera")
      desired["stitching"]["camera_fov"]["horizontal_fov"] = 100;
    if (kind == "backend") {
      desired["stitching"]["mapping_backend"] = "opencv-magsac";
      desired["stitching"]["projection"] = "rectilinear";
    }
    if (kind == "count")
      desired["stitching"]["calibration_frame_count"] = 3;
    if (kind == "sync")
      desired["stitching"]["sync_method"] = "imu";
    if (kind == "time") {
      desired["stitching"]["stitch_frame_time"] = "00:00:01";
      const auto original = YAML::Dump(desired);
      const auto result = st::PrepareStitchingCalibrationControlsLocked(
          game,
          take(st::ReadStitchingCalibrationState(before, before)),
          take(st::ReadStitchingCalibrationControls(desired)),
          before,
          desired,
          "new-owner",
          false,
          false,
          false,
          false,
          desired);
      require(
          !result.ok() && YAML::Dump(desired) == original,
          "Conflicting selected reference time must fail without mutation");
      before["stitching"].remove("calibration_frame_selection");
      before["stitching"].remove("calibration_frame_inputs_fingerprint");
      desired["stitching"].remove("calibration_frame_selection");
      desired["stitching"].remove("calibration_frame_inputs_fingerprint");
    }
    const auto plan = prepare(game, before, desired, kind == "sources");
    const bool input = kind == "count" || kind == "sync" || kind == "time" || kind == "sources";
    const bool features = kind == "budget" || kind == "matcher" || kind == "provider";
    require(
        plan.required &&
            plan.start_stage ==
                (input          ? "input"
                     : features ? "features"
                                : "canvas"),
        "Shared planner selected the wrong dependency stage");
    require(
        plan.retain_selected_frames == (kind != "count" && kind != "time" && kind != "sources"),
        "Shared planner changed exact selected-frame retention");
    require(
        plan.retain_manual_matches == (kind == "backend" || kind == "sync"),
        "Shared planner changed manual-match retention");
    const auto cleanup = kind == "count" || kind == "time" || kind == "sources"
        ? st::StitchingCalibrationCleanup::kAll
        : st::StitchingCalibrationCleanup::kControlPoints;
    require(plan.cleanup == cleanup, "Shared planner selected the wrong cleanup scope");
    if (kind == "sources")
      require(
          !desired["rink"]["mask_frame"]["input_id"] && !desired["stitching"]["frame_offsets"],
          "Source-role edits retained incompatible mask or synchronization ownership");
  }
  {
    auto before = baseline(), desired = YAML::Clone(before);
    desired["stitching"]["projection_framing"]["crop"] = std::vector<double>{0.1, 0.9, 0.1, 0.9};
    const auto original = YAML::Dump(desired);
    const auto result = st::PrepareStitchingCalibrationControlsLocked(
        game,
        take(st::ReadStitchingCalibrationState(before, before)),
        take(st::ReadStitchingCalibrationControls(desired)),
        before,
        desired,
        "new-owner",
        false,
        false,
        false,
        false,
        desired);
    require(
        !result.ok() && YAML::Dump(desired) == original,
        "A view change without verified saved alignment must not fall back to a fresh solve");
  }
  {
    auto before = baseline();
    before["hstream_ui"]["stitching_calibration"]["status"] = "pending";
    before["hstream_ui"]["stitching_calibration"]["stale_from"] = "input";
    auto desired = YAML::Clone(before);
    desired["stitching"]["projection_framing"]["crop"] = std::vector<double>{0.1, 0.9, 0.1, 0.9};
    const auto plan = prepare(game, before, desired);
    require(
        plan.required && !plan.alignment_reused && plan.start_stage == "input" &&
            plan.cleanup == st::StitchingCalibrationCleanup::kAll,
        "An uncalibrated source must accept geometry controls without claiming a saved alignment");
  }
  {
    auto before = baseline();
    before["hstream_ui"]["stitching_calibration"]["status"] = "pending";
    before["hstream_ui"]["stitching_calibration"]["stale_from"] = "features";
    before["hstream_ui"]["stitching_calibration"]["artifacts_invalidated"] = true;
    auto desired = YAML::Clone(before);
    const auto plan = prepare(game, before, desired);
    require(
        plan.required && plan.start_stage == "features" && plan.cleanup == st::StitchingCalibrationCleanup::kNone,
        "Already cleaned pending features must resume without deleting retained inputs");
  }
}

void cleanup(const fs::path& game) {
  auto before = baseline(), desired = YAML::Clone(before);
  desired["hstream_ui"]["stitching_calibration"]["control_points"] = 1600;
  const auto plan = prepare(game, before, desired);
  write(game / "config.yaml", YAML::Dump(desired));
  for (const char* name : {"left.png", "right.png", "hm_project.pto", "mapping_0000.tif", "rink_mask_0.png"})
    write(game / name, "old artifact");
  write(game / "player-frame-inputs" / "historical" / "input.png", "immutable input");
  write(game / "rink-mask-input" / "historical" / "manifest.yaml", "immutable provenance");
  check(st::clean_stitching_artifacts_from_control_points(game.string(), plan.owner));
  require(
      fs::exists(game / "left.png") && fs::exists(game / "right.png") && !fs::exists(game / "hm_project.pto") &&
          !fs::exists(game / "mapping_0000.tif") && !fs::exists(game / "rink_mask_0.png"),
      "Native cleanup retained the wrong dependency artifacts");
  auto superseded = YAML::LoadFile((game / "config.yaml").string());
  superseded["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "newer-owner";
  write(game / "config.yaml", YAML::Dump(superseded));
  require(
      !st::CompleteStitchingCalibrationCleanup(game, plan, desired).ok(), "Superseded cleanup completed a stale owner");
  superseded["hstream_ui"]["stitching_calibration"]["invalidation_id"] = plan.owner;
  superseded["stitching"]["sync_method"] = "imu";
  write(game / "config.yaml", YAML::Dump(superseded));
  require(
      !st::CompleteStitchingCalibrationCleanup(game, plan, desired).ok(),
      "A changed control must reject cleanup completion even if its owner is unchanged");
  superseded["stitching"]["sync_method"] = desired["stitching"]["sync_method"];
  write(game / "config.yaml", YAML::Dump(superseded));
  check(st::CompleteStitchingCalibrationCleanup(game, plan, desired));
  require(
      YAML::LoadFile((game / "config.yaml").string())["hstream_ui"]["stitching_calibration"]["artifacts_invalidated"]
          .as<bool>(),
      "Native cleanup did not publish its checked completion");
  require(
      fs::exists(game / "player-frame-inputs" / "historical" / "input.png") &&
          fs::exists(game / "rink-mask-input" / "historical" / "manifest.yaml"),
      "Cleanup deleted immutable historical input bundles");
}
} // namespace

int main() {
  char path[] = "/tmp/stitching-controls-test-XXXXXX";
  const char* root = ::mkdtemp(path);
  if (!root)
    return 1;
  try {
    matrix(root);
    cleanup(root);
    fs::remove_all(root);
    std::cout << "Shared calibration dependency, retention and guarded cleanup cases passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    fs::remove_all(root);
    return 1;
  }
}
