#include "hstream/src/libs/stitching/StitchingReframe.h"

#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>

#include <opencv2/imgcodecs.hpp>

#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/PlayerFrameInputStore.h"

namespace fs = std::filesystem;
using namespace hm::stitching;

namespace {

class ScopedEnvironment {
 public:
  ScopedEnvironment(const char* name, const char* replacement) : name_(name) {
    if (const char* previous = std::getenv(name))
      previous_ = previous;
    if (::setenv(name, replacement, 1) != 0)
      throw std::runtime_error("Cannot set fixture environment");
  }
  ~ScopedEnvironment() {
    if (previous_)
      ::setenv(name_.c_str(), previous_->c_str(), 1);
    else
      ::unsetenv(name_.c_str());
  }

 private:
  std::string name_;
  std::optional<std::string> previous_;
};

bool expect(bool value, const std::string& message) {
  if (!value)
    std::cerr << "FAIL: " << message << '\n';
  return value;
}

bool test_optional_presence() {
  bool ok = true;
  for (const char* contents :
       {"",
        "{}",
        "stitching: {}",
        "hstream_ui: null",
        "hstream_ui: value",
        "hstream_ui: {stitching_calibration: []}",
        "hstream_ui: {stitching_calibration: {reframe: null}}"}) {
    auto config = YAML::Load(contents);
    ok &= expect(!HasStitchingReframeIntent(config), "absent intent is safe through missing or malformed parents");
    ClearStitchingReframeIntent(config);
  }
  for (const char* contents : {"reframe: invalid", "reframe: []", "reframe: {}"}) {
    YAML::Node config;
    config["hstream_ui"]["stitching_calibration"] = YAML::Load(contents);
    ok &= expect(HasStitchingReframeIntent(config), "malformed non-null intent remains explicit");
    ClearStitchingReframeIntent(config);
    ok &= expect(!HasStitchingReframeIntent(config), "explicit clear removes intent");
  }
  return ok;
}

void require(const absl::Status& status) {
  if (!status.ok())
    throw std::runtime_error(status.ToString());
}

template <typename T>
T value(absl::StatusOr<T> result) {
  require(result.status());
  return std::move(*result);
}

void write(const fs::path& path, const std::string& bytes) {
  std::ofstream(path, std::ios::binary) << bytes;
}

StitchingBackendChoices choices(const YAML::Node& config) {
  StitchingBackendChoices result;
  result.control_point_matcher = "superpoint-lightglue";
  result.mapping_backend = "nona";
  result.projection = config["stitching"]["projection"].as<std::string>();
  result.run_autooptimizer = true;
  result.projection_parameters =
      value(read_stitch_projection_parameters(config, value(ParseStitchProjection(result.projection))));
  result.projection_framing = value(read_stitch_projection_framing(config));
  result.camera = value(read_stitch_camera_selection(config));
  result.control_point_resolution = value(read_control_point_resolution(config));
  result.control_point_execution_provider = value(read_control_point_execution_provider(config));
  result.calibration_frame_selection_fingerprint = value(player_frame_selection_fingerprint(config));
  return result;
}

YAML::Node fixture(const fs::path& root) {
  fs::create_directories(root);
  auto config = YAML::Clone(value(hm::baseline_config::load()).values);
  config["stitching"]["calibration_frame_count"] = 2;
  config["stitching"]["control_point_resolution"] = "native";
  config["stitching"]["control_point_execution_provider"] = "cpu";
  config["stitching"]["max_output_width"] = 0;
  config["stitching"]["max_output_dimension"] = 0;
  config["stitching"]["rink_config"] = "";
  StitchProjectionFraming framing;
  framing.rotation_degrees = {0, -15, 3};
  write_stitch_projection_framing(config, framing);
  write_stitch_camera_selection(config, {"gopro-mission-1", 109, 64});
  auto state = config["hstream_ui"]["stitching_calibration"];
  state["status"] = "complete";
  state["invalidation_id"] = "completed-solve";
  state["control_points"] = 1500;
  state["frame_count"] = 2;
  require(reserve_stitching_backend_generation_in_config(config, "completed-solve", choices(config)));
  for (const char* name : {"hm_project.pto", "autooptimiser_out.pto"})
    write(
        root / name,
        "p f19 w64 h32 v180\ni w16 h16 f0 v109 r0 p0 y0 n\"left.png\"\ni w16 h16 f0 v109 r0 p0 y45 n\"right.png\"\nc n0 N1 x1 y2 X3 Y4 t0\n");
  for (const char* name :
       {"mapping_0000.tif",
        "mapping_0000_x.tif",
        "mapping_0000_y.tif",
        "mapping_0001.tif",
        "mapping_0001_x.tif",
        "mapping_0001_y.tif"})
    if (!cv::imwrite((root / name).string(), cv::Mat(32, 64, CV_16UC1, cv::Scalar(1))))
      throw std::runtime_error("Cannot create TIFF fixture");
  for (const char* name : {"left.png", "right.png", "seam_file.png"})
    if (!cv::imwrite((root / name).string(), cv::Mat(16, 16, CV_8UC3, cv::Scalar(1, 2, 3))))
      throw std::runtime_error("Cannot create PNG fixture");
  return config;
}

void provenance(
    const fs::path& root,
    const std::string& fingerprint = {},
    const std::string& diagnostics = {},
    StitchProjection projection = StitchProjection::kGeneralPanini,
    double horizontal_fov = 180) {
  const auto parameters = FormatStitchProjectionParameters(DefaultStitchProjectionParameters(projection));
  write(
      root / kStitchCanvasProvenanceArtifact,
      "version=10\nmax-output-width=0\nmax-canvas-dimension=0\nsource-canvas-width=64\nsource-canvas-height=32\n"
      "canvas-width=64\ncanvas-height=32\nmax-output-width-applied=0\nmax-canvas-dimension-applied=0\n"
      "mapping-backend=nona\nprojection=" +
          std::string(StitchProjectionName(projection)) +
          "\nprojection-parameters=" + (parameters.empty() ? "none" : parameters) +
          "\nprojection-auto-fov=0\n"
          "projection-horizontal-fov=" +
          std::to_string(horizontal_fov) +
          "\nprojection-auto-canvas=1\nprojection-auto-crop=0\n"
          "camera-configuration=gopro-mission-1\ncamera-horizontal-fov=109\ncamera-vertical-fov=64\n"
          "control-point-matcher=superpoint-lightglue\nakaze-calibration-fingerprint=not-applicable\n"
          "projection-rotation-0=0\nprojection-rotation-1=-15\nprojection-rotation-2=3\n"
          "projection-crop-0=0\nprojection-crop-1=1\nprojection-crop-2=0\nprojection-crop-3=1\n"
          "control-point-resolution=native\ncalibration-frame-selection=" +
          (fingerprint.empty() ? "none" : fingerprint) +
          "\ncalibration-frame-diagnostics=" + (diagnostics.empty() ? "none" : diagnostics) + "\n");
}

void pending(YAML::Node& config, const std::string& owner) {
  auto state = config["hstream_ui"]["stitching_calibration"];
  state["status"] = "pending";
  state["stale_from"] = "canvas";
  state["invalidation_id"] = owner;
  state.remove("backend_generation");
}

YAML::Node desired_view(const YAML::Node& before) {
  auto desired = YAML::Clone(before);
  auto framing = value(read_stitch_projection_framing(desired));
  framing.rotation_degrees = {0, -20, 4};
  framing.crop = {0.1, 0.9, 0.05, 0.95};
  write_stitch_projection_framing(desired, framing);
  return desired;
}

bool test_ownership_and_edits(const fs::path& root) {
  auto before = fixture(root);
  provenance(root);
  auto artifact_lock = value(lock_canvas_constraint_artifacts(root));
  auto config_lock = value(GameConfigTransactionLock::Acquire(root));
  auto desired = desired_view(before);
  auto prepare = PrepareStitchingReframeIntentLocked(root, before, desired, "view-1", desired);
  bool ok = expect(prepare.ok() && *prepare, "geometry edit captures the original complete alignment");
  if (!prepare.ok()) {
    std::cerr << prepare.status() << '\n';
    return false;
  }
  pending(desired, "view-1");
  auto valid = ValidateStitchingReframeIntentLocked(root, desired);
  ok &= expect(valid.ok(), "captured reframe validates under its new pending owner");
  if (!valid.ok()) {
    std::cerr << valid.status() << '\n';
    return false;
  }
  ok &= expect(
      valid->source_provenance.projection_framing->rotation_degrees == std::array<double, 3>{0, -15, 3},
      "source rotation stays bound to original accepted view");
  auto rebound = YAML::Clone(desired);
  rebound["stitching"]["max_output_width"] = 50;
  prepare = PrepareStitchingReframeIntentLocked(root, desired, rebound, "view-2", rebound);
  ok &= expect(prepare.ok() && *prepare, "another view edit rebinds intent");
  pending(rebound, "view-2");
  auto second = ValidateStitchingReframeIntentLocked(root, rebound);
  ok &= expect(
      second.ok() && second->source_generation == valid->source_generation && second->desired_owner == "view-2" &&
          second->desired_max_output_width == 50,
      "rebind retains exact original source generation");
  auto mismatched = YAML::Clone(rebound);
  mismatched["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "other";
  ok &= expect(!ValidateStitchingReframeIntentLocked(root, mismatched).ok(), "foreign owner cannot consume intent");
  mismatched = YAML::Clone(rebound);
  mismatched["hstream_ui"]["stitching_calibration"]["stale_from"] = "features";
  ok &= expect(
      !ValidateStitchingReframeIntentLocked(root, mismatched).ok(),
      "features invalidation cannot masquerade as reframe");
  for (const auto& change : std::vector<std::pair<std::string, std::string>>{
           {"control_point_execution_provider", "cuda"},
           {"control_point_resolution", "2k"},
           {"stitch_frame_time", "00:01:00"}}) {
    auto edited = YAML::Clone(rebound);
    edited["stitching"][change.first] = change.second;
    ok &= expect(
        !ValidateStitchingReframeIntentLocked(root, edited).ok(),
        "out-of-band solve edit fails validation: " + change.first);
    auto requested = YAML::Clone(edited);
    prepare = PrepareStitchingReframeIntentLocked(root, rebound, edited, "new-solve", requested);
    ok &= expect(
        prepare.ok() && !*prepare && !HasStitchingReframeIntent(requested),
        "explicit solve edit atomically clears intent: " + change.first);
  }
  auto fixed = YAML::Clone(rebound);
  fixed["stitching"]["projection_framing"]["auto_canvas"] = false;
  prepare = PrepareStitchingReframeIntentLocked(root, rebound, fixed, "fixed", fixed);
  ok &=
      expect(!prepare.ok() && HasStitchingReframeIntent(fixed), "unsupported fixed canvas fails without losing intent");
  auto malformed = YAML::Clone(rebound);
  malformed["hstream_ui"]["stitching_calibration"]["reframe"] = "invalid";
  ok &= expect(
      HasStitchingReframeIntent(malformed) && !ValidateStitchingReframeIntentLocked(root, malformed).ok(),
      "malformed explicit request cannot become ordinary calibration");
  write(root / "left.png", "replaced representative");
  ok &= expect(
      !ValidateStitchingReframeIntentLocked(root, rebound).ok(),
      "representative replacement is detected outside generation fingerprint");
  return ok;
}

bool test_capture_guards(const fs::path& root) {
  auto before = fixture(root);
  provenance(root);
  auto artifact_lock = value(lock_canvas_constraint_artifacts(root));
  auto config_lock = value(GameConfigTransactionLock::Acquire(root));
  auto missing_canonical_count = YAML::Clone(before);
  missing_canonical_count["stitching"].remove("calibration_frame_count");
  auto inherited_count_edit = desired_view(missing_canonical_count);
  auto inherited_count_capture = PrepareStitchingReframeIntentLocked(
      root, missing_canonical_count, inherited_count_edit, "saved-count", inherited_count_edit);
  bool ok = expect(
      inherited_count_capture.ok() && *inherited_count_capture,
      "saved count remains authoritative when the canonical value would inherit a different baseline");
  auto legacy = YAML::Clone(before);
  legacy["hstream_ui"]["stitching_calibration"].remove("control_points");
  legacy["hstream_ui"]["stitching_calibration"].remove("frame_count");
  auto unchanged = YAML::Clone(legacy);
  auto prepare = PrepareStitchingReframeIntentLocked(root, legacy, unchanged, "unused", unchanged);
  ok &= expect(prepare.ok() && !*prepare, "unchanged legacy playback does not require capture-grade config");
  auto edited = desired_view(legacy);
  prepare = PrepareStitchingReframeIntentLocked(root, legacy, edited, "missing", edited);
  ok &= expect(!prepare.ok(), "missing original count/budget cannot be inferred for geometry reuse");
  auto pending_source = YAML::Clone(before);
  pending(pending_source, "features-owner");
  pending_source["hstream_ui"]["stitching_calibration"]["stale_from"] = "features";
  edited = desired_view(pending_source);
  prepare = PrepareStitchingReframeIntentLocked(root, pending_source, edited, "later-geometry", edited);
  ok &= expect(
      prepare.ok() && !*prepare && !HasStitchingReframeIntent(edited),
      "pending features never becomes a completed source");
  auto mismatched = YAML::Clone(before);
  mismatched["stitching"]["projection_framing"]["rotation_degrees"] = std::vector<double>{0, 0, 0};
  edited = desired_view(mismatched);
  prepare = PrepareStitchingReframeIntentLocked(root, mismatched, edited, "wrong-source", edited);
  ok &= expect(!prepare.ok(), "pre-edit source must agree with completed backend claim");
  edited = desired_view(before);
  fs::remove(root / kStitchCanvasProvenanceArtifact);
  prepare = PrepareStitchingReframeIntentLocked(root, before, edited, "missing-provenance", edited);
  ok &= expect(!prepare.ok(), "missing source provenance fails without authorizing matching");
  unchanged = YAML::Clone(legacy);
  prepare = PrepareStitchingReframeIntentLocked(root, legacy, unchanged, "unused", unchanged);
  ok &= expect(
      prepare.ok() && !*prepare, "unchanged complete legacy config without provenance remains ordinary playback");
  return ok;
}

bool test_environment_canvas_limit(const fs::path& root) {
  auto before = fixture(root);
  provenance(root);
  auto artifact_lock = value(lock_canvas_constraint_artifacts(root));
  auto config_lock = value(GameConfigTransactionLock::Acquire(root));
  const auto source_generation = value(stitch_artifact_generation_id_locked(root));
  ScopedEnvironment allow_limit("HM_ALLOW_OVERSIZED_LIVE_STITCH", "0");
  ScopedEnvironment canvas_limit("HM_MAX_LIVE_STITCH_EGL_DIMENSION", "48");
  auto desired = YAML::Clone(before);
  const auto prepared = PrepareStitchingReframeIntentLocked(root, before, desired, "new-runtime-limit", desired);
  bool ok = expect(prepared.ok() && *prepared, "environment-only canvas limit change captures saved alignment");
  if (!prepared.ok()) {
    std::cerr << prepared.status() << '\n';
    return false;
  }
  pending(desired, "new-runtime-limit");
  const auto validated = ValidateStitchingReframeIntentLocked(root, desired);
  ok &= expect(
      validated.ok() && validated->desired_max_output_dimension == 48 &&
          validated->source_provenance.max_canvas_dimension == 0 && validated->source_generation == source_generation,
      "environment reframe freezes published cap and source identity while requesting new cap");
  ok &= expect(
      value(stitch_artifact_generation_id_locked(root)) == source_generation,
      "capturing an environment-only view change retains original artifact generation");
  ScopedEnvironment newer_limit("HM_MAX_LIVE_STITCH_EGL_DIMENSION", "32");
  ok &= expect(
      !ValidateStitchingReframeIntentLocked(root, desired).ok(),
      "a later runtime canvas limit cannot silently replace the saved request");
  return ok;
}

bool test_completed_runtime_reservation(const fs::path& root) {
  auto completed = fixture(root);
  provenance(root);
  auto artifact_lock = value(lock_canvas_constraint_artifacts(root));
  auto config_lock = value(GameConfigTransactionLock::Acquire(root));
  const auto generation = value(stitch_artifact_generation_id_locked(root));
  // Unchanged Play reserves this token before its runner starts. A launch
  // failure can leave the completed backend claim under its original owner.
  auto reserved = YAML::Clone(completed);
  reserved["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "runner-never-started";
  auto unchanged = YAML::Clone(reserved);
  const auto reused = PrepareStitchingReframeIntentLocked(root, reserved, unchanged, "unused", unchanged);
  bool ok = expect(reused.ok() && !*reused, "runtime reservation alone does not request a new alignment");
  auto desired = desired_view(reserved);
  const auto prepared = PrepareStitchingReframeIntentLocked(root, reserved, desired, "after-launch-failure", desired);
  ok &= expect(prepared.ok() && *prepared, "failed unchanged-Play launch cannot strand a completed alignment");
  if (!prepared.ok()) {
    std::cerr << prepared.status() << '\n';
    return false;
  }
  ok &= expect(
      desired["hstream_ui"]["stitching_calibration"]["reframe"]["source_owner"].as<std::string>() == "completed-solve",
      "source owner comes exclusively from the original completed backend claim");
  pending(desired, "after-launch-failure");
  const auto validated = ValidateStitchingReframeIntentLocked(root, desired);
  ok &= expect(
      validated.ok() && validated->source_generation == generation,
      "request captured after failed launch validates against unchanged published source");
  auto changed_source = YAML::Clone(reserved);
  changed_source["stitching"]["control_point_execution_provider"] = "cuda";
  auto changed_desired = desired_view(changed_source);
  ok &= expect(
      !PrepareStitchingReframeIntentLocked(root, changed_source, changed_desired, "wrong-provider", changed_desired)
           .ok(),
      "separate runtime owner cannot hide a completed solver tuple mutation");
  changed_source = YAML::Clone(reserved);
  changed_source["hstream_ui"]["stitching_calibration"]["backend_generation"]["camera_fov"]["horizontal_fov"] = 110;
  changed_desired = desired_view(changed_source);
  ok &= expect(
      !PrepareStitchingReframeIntentLocked(root, changed_source, changed_desired, "wrong-claim", changed_desired).ok(),
      "separate runtime owner cannot hide a mutated original backend claim");
  auto pending_source = YAML::Clone(reserved);
  pending_source["hstream_ui"]["stitching_calibration"]["status"] = "pending";
  pending_source["hstream_ui"]["stitching_calibration"]["stale_from"] = "features";
  changed_desired = desired_view(pending_source);
  const auto pending_request =
      PrepareStitchingReframeIntentLocked(root, pending_source, changed_desired, "still-pending", changed_desired);
  ok &= expect(
      pending_request.ok() && !*pending_request && !HasStitchingReframeIntent(changed_desired),
      "original backend claim never authorizes adoption of a pending source");
  return ok;
}

bool test_projections_without_parameters(const fs::path& root) {
  bool ok = true;
  for (const auto& projection : SupportedStitchProjections()) {
    if (!StitchProjectionParameters(projection.projection).empty())
      continue;
    const fs::path game = root / projection.name;
    auto before = fixture(game);
    before["stitching"]["projection"] = projection.name;
    auto framing = value(read_stitch_projection_framing(before));
    framing.horizontal_fov = 120;
    write_stitch_projection_framing(before, framing);
    before["hstream_ui"]["stitching_calibration"].remove("backend_generation");
    require(reserve_stitching_backend_generation_in_config(before, "completed-solve", choices(before)));
    provenance(game, {}, {}, projection.projection, framing.horizontal_fov);
    for (const char* name : {"hm_project.pto", "autooptimiser_out.pto"})
      write(
          game / name,
          "p f" + std::to_string(projection.hugin_projection) +
              " w64 h32 v120\n"
              "i w16 h16 f0 v109 r0 p0 y0 n\"left.png\"\ni w16 h16 f0 v109 r0 p0 y45 n\"right.png\"\nc n0 N1 x1 y2 X3 Y4 t0\n");
    auto artifact_lock = value(lock_canvas_constraint_artifacts(game));
    auto config_lock = value(GameConfigTransactionLock::Acquire(game));
    auto desired = desired_view(before);
    const auto prepared = PrepareStitchingReframeIntentLocked(game, before, desired, "saved-view", desired);
    ok &= expect(prepared.ok() && *prepared, std::string(projection.name) + " source captures without parameters");
    if (!prepared.ok()) {
      std::cerr << prepared.status() << '\n';
      continue;
    }
    pending(desired, "saved-view");
    const auto validated = ValidateStitchingReframeIntentLocked(game, desired);
    ok &= expect(
        validated.ok() && validated->desired_choices.projection_parameters.empty(),
        std::string(projection.name) + " source reconstructs its omitted parameter entry");
    auto retry = YAML::Clone(desired);
    retry["stitching"]["max_output_width"] = 48;
    const auto rebound = PrepareStitchingReframeIntentLocked(game, desired, retry, "retry-view", retry);
    ok &= expect(rebound.ok() && *rebound, std::string(projection.name) + " saved view rebinds on subsequent Play");
    if (!rebound.ok()) {
      std::cerr << rebound.status() << '\n';
      continue;
    }
    pending(retry, "retry-view");
    ok &= expect(
        ValidateStitchingReframeIntentLocked(game, retry).ok(),
        std::string(projection.name) + " rebound intent preserves valid canonical parameter syntax");
  }
  return ok;
}

PlayerFrameSelectionPlan selected_plan(const fs::path& root) {
  PlayerFrameSelectionPlan plan;
  plan.settings.frame_count = 2;
  plan.context = {
      {"source_context", "frozen-chapters"},
      {"baseline_generation", "baseline"},
      {"output_generation", "output"},
      {"detector_identity", "detector"},
      {"rink_mask_sha256", "mask"},
      {"rink_mask_revision", "mask-revision"},
      {"fieldmask_settings", "rink"},
      {"output_rotation_degrees", "0"}};
  for (const char* role : {"left", "right"}) {
    write(root / (std::string(role) + ".mp4"), "recorded-media");
    plan.sources.push_back(value(BindPlayerFrameSource(root / (std::string(role) + ".mp4"))));
  }
  for (size_t index = 0; index < 2; ++index) {
    PlayerFrameObservation selected;
    selected.pair.timeline_pts_ns = (10 + index * 2) * kPlayerFrameSecond;
    for (size_t camera = 0; camera < 2; ++camera)
      selected.pair.cameras[camera] = {
          plan.sources[camera].path, selected.pair.timeline_pts_ns, static_cast<uint32_t>(camera), index * 60};
    if (index > 0) {
      selected.eligible_people = 1;
      selected.size_band_counts[0] = 1;
      selected.coverage = {7};
      selected.quality = 0.9;
    }
    plan.selected.push_back(selected);
  }
  plan.fingerprint = value(PlayerFrameSelectionFingerprint(plan));
  require(ValidatePlayerFrameSelectionPlan(plan));
  return plan;
}

bool test_selected_retention(const fs::path& root) {
  auto before = fixture(root);
  const auto plan = selected_plan(root);
  before["stitching"]["calibration_frame_selection"] = PlayerFrameSelectionPlanYaml(plan);
  before["stitching"]["calibration_frame_inputs_fingerprint"] = plan.fingerprint;
  before["hstream_ui"]["stitching_calibration"].remove("backend_generation");
  require(reserve_stitching_backend_generation_in_config(before, "completed-solve", choices(before)));
  std::vector<std::array<fs::path, 2>> images(2, {root / "left.png", root / "right.png"});
  for (size_t camera = 0; camera < 2; ++camera) {
    images[0][camera] = root / (camera ? "first-right.png" : "first-left.png");
    if (!cv::imwrite(images[0][camera].string(), cv::Mat(16, 16, CV_8UC3, cv::Scalar(4, 5, 6))))
      throw std::runtime_error("Cannot create alternate representative fixture");
  }
  require(PublishPlayerFrameInputs(root, plan, images));
  provenance(root, plan.fingerprint, "representative=0;pooled=0;selected=16");
  auto artifact_lock = value(lock_canvas_constraint_artifacts(root));
  auto config_lock = value(GameConfigTransactionLock::Acquire(root));
  auto desired = desired_view(before);
  bool ok = expect(
      !PrepareStitchingReframeIntentLocked(root, before, desired, "wrong-pair", desired).ok(),
      "representative diagnostics must identify the exact retained public PNG pair");
  provenance(root, plan.fingerprint, "representative=1;pooled=0;selected=16");
  const auto prepared = PrepareStitchingReframeIntentLocked(root, before, desired, "selected-view", desired);
  ok &= expect(prepared.ok() && *prepared, "selected retained pair authorizes reframe");
  if (!prepared.ok()) {
    std::cerr << prepared.status() << '\n';
    return false;
  }
  pending(desired, "selected-view");
  fs::remove(root / "left.mp4");
  fs::remove(root / "right.mp4");
  ok &= expect(
      ValidateStitchingReframeIntentLocked(root, desired).ok(),
      "reframe uses retained inputs with original recordings unavailable");
  const auto bundle = value(LoadPlayerFrameInputs(root, plan));
  fs::remove(bundle->images[1][0]);
  ok &= expect(
      !ValidateStitchingReframeIntentLocked(root, desired).ok(),
      "missing selected PNG fails even when public stills remain valid");
  return ok;
}

} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("stitching-reframe-test-" + std::to_string(::getpid()));
  try {
    ScopedEnvironment no_platform_limit("HM_ALLOW_OVERSIZED_LIVE_STITCH", "1");
    bool ok = test_optional_presence();
    ok &= test_ownership_and_edits(root / "ownership");
    ok &= test_capture_guards(root / "capture");
    ok &= test_environment_canvas_limit(root / "environment");
    ok &= test_completed_runtime_reservation(root / "runtime-reservation");
    ok &= test_projections_without_parameters(root / "no-parameters");
    ok &= test_selected_retention(root / "selected");
    fs::remove_all(root);
    return ok ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    fs::remove_all(root);
    return 1;
  }
}
