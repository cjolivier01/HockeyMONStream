#include "hstream/src/libs/stitching/StitchingReframe.h"

#include <unistd.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>

#include <opencv2/imgcodecs.hpp>
#include <tiffio.h>

#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/ConfigureStitchingCore.h"
#include "hstream/src/libs/stitching/PlayerFrameInputStore.h"
#include "hstream/src/libs/stitching/StitchingCalibrationControls.h"

namespace fs = std::filesystem;
using namespace hm::stitching;

namespace {
std::filesystem::path saved_state(const std::filesystem::path& game) {
  const auto state = game / "_hockeymon";
  std::filesystem::create_directories(state);
  return state;
}
} // namespace

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
  result.control_point_matcher = ControlPointMatcherName(value(resolve_control_point_matcher(config)));
  result.mapping_backend = "nona";
  result.projection = config["stitching"]["projection"].as<std::string>();
  result.run_autooptimizer = true;
  result.projection_parameters =
      value(read_stitch_projection_parameters(config, value(ParseStitchProjection(result.projection))));
  result.projection_framing = value(read_stitch_projection_framing(config));
  result.camera = value(read_stitch_camera_selection(config));
  result.control_point_resolution = value(read_control_point_resolution(config));
  result.control_point_filtering = value(read_control_point_filtering(config));
  result.control_point_execution_provider = value(read_control_point_execution_provider(config));
  result.calibration_frame_selection_fingerprint = value(player_frame_selection_fingerprint(config));
  result.manual_control_point_fingerprint = value(manual_control_point_fingerprint(config));
  return result;
}

YAML::Node fixture(const fs::path& root) {
  fs::create_directories(root);
  auto config = YAML::Clone(value(hm::baseline_config::load()).values);
  config["stitching"]["calibration_frame_count"] = 2;
  // Pin the matcher so this fixture keeps exercising a resolution-bearing
  // neural backend rather than following the shipped baseline default.
  config["stitching"]["control_point_matcher"] = "superpoint-lightglue";
  config["stitching"]["control_point_resolution"] = "native";
  write_control_point_filtering(config, {false, false});
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
        saved_state(root) / name,
        "p f19 w64 h32 v180\ni w16 h16 f0 v109 r0 p0 y0 n\"left.png\"\ni w16 h16 f0 v109 r0 p0 y45 n\"right.png\"\nc n0 N1 x1 y2 X3 Y4 t0\n");
  for (const char* name :
       {"mapping_0000.tif",
        "mapping_0000_x.tif",
        "mapping_0000_y.tif",
        "mapping_0001.tif",
        "mapping_0001_x.tif",
        "mapping_0001_y.tif"})
    if (!cv::imwrite((saved_state(root) / name).string(), cv::Mat(32, 64, CV_16UC1, cv::Scalar(1))))
      throw std::runtime_error("Cannot create TIFF fixture");
  for (const char* name : {"left.png", "right.png", "seam_file.png"})
    if (!cv::imwrite((saved_state(root) / name).string(), cv::Mat(16, 16, CV_8UC3, cv::Scalar(1, 2, 3))))
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
      saved_state(root) / kStitchCanvasProvenanceArtifact,
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

bool test_serialized_intent(const fs::path& root) {
  auto before = fixture(root);
  before["game"]["videos"]["left"].push_back("1");
  before["game"]["videos"]["right"].push_back("002");
  before["game"]["stitching"]["frame_offsets"]["left"] = 1;
  before["game"]["stitching"]["frame_offsets"]["right"] = 0.885511;
  provenance(root);
  auto artifact_lock = value(lock_canvas_constraint_artifacts(root));
  auto config_lock = value(GameConfigTransactionLock::Acquire(root));
  auto desired = desired_view(before);
  const auto prepared = PrepareStitchingReframeIntentLocked(root, before, desired, "serialized-view", desired);
  bool ok = expect(prepared.ok() && *prepared, "capture intent before launch serialization");
  if (!prepared.ok() || !*prepared)
    return false;
  pending(desired, "serialized-view");
  auto launch = YAML::Load(YAML::Dump(desired));
  auto record = launch["hstream_ui"]["stitching_calibration"]["reframe"];
  // The outer service loads native staging YAML, then emits launch YAML with
  // PyYAML's shortest float spellings. These are the actual 5% crop spellings
  // from the failed projection-edit playback, plus equivalent solver values.
  record["desired_view"]["projection_framing"]["crop"] = YAML::Load("[0.1, 0.9, 0.05, 0.95]");
  record["desired_view"]["projection_parameters"][0] = YAML::Load("1.0e+2");
  record["source_solver"]["camera_fov"]["horizontal_fov"] = YAML::Load("109.0");
  record["source_solver"]["frame_offsets"]["left"] = YAML::Load("1.0");
  record["source_solver"]["frame_offsets"]["right"] = YAML::Load("0.885511");
  write(saved_state(root) / "config.yaml", YAML::Dump(launch));
  const auto roundtrip = YAML::LoadFile((saved_state(root) / "config.yaml").string());
  const auto valid = ValidateStitchingReframeIntentLocked(root, roundtrip);
  ok &= expect(valid.ok(), "runtime accepts equal numeric values after outer launch serialization");
  if (!valid.ok())
    std::cerr << valid.status() << '\n';
  for (const char* change : {"crop", "offset", "video", "video-alias", "unknown", "infinite", "malformed"}) {
    auto changed = YAML::Clone(roundtrip);
    auto intent = changed["hstream_ui"]["stitching_calibration"]["reframe"];
    const std::string name(change);
    if (name == "crop")
      intent["desired_view"]["projection_framing"]["crop"][2] = std::nextafter(0.05, 1.0);
    else if (name == "offset")
      intent["source_solver"]["frame_offsets"]["left"] = std::nextafter(1.0, 2.0);
    else if (name == "video")
      intent["source_solver"]["videos"]["left"][0] = "01";
    else if (name == "video-alias") {
      // Normalizing the numeric anchor must not rewrite an aliased filename.
      intent["source_solver"]["frame_offsets"]["left"] = YAML::Load("01");
      intent["source_solver"]["videos"]["left"][0] = intent["source_solver"]["frame_offsets"]["left"];
    } else if (name == "unknown")
      intent["desired_view"]["projection_framing"]["new_setting"] = 0;
    else
      intent["desired_view"]["projection_framing"]["crop"][2] = name == "infinite" ? ".inf" : "invalid";
    ok &= expect(
        !ValidateStitchingReframeIntentLocked(root, changed).ok(),
        "launch serialization must not weaken changed or invalid " + name + " checks");
    auto rejected = YAML::Clone(changed);
    ok &= expect(
        !PrepareResolvedStitchingReframeIntentLocked(root, changed, changed, "invalid-rebind", rejected).ok(),
        "resolved staging must retain exact identity checks for " + name);
  }
  auto another = YAML::Clone(roundtrip);
  another["stitching"]["max_output_width"] = 50;
  const auto rebound = PrepareStitchingReframeIntentLocked(root, roundtrip, another, "serialized-retry", another);
  ok &= expect(rebound.ok() && *rebound, "serialized intent can be rebound without losing its saved alignment");
  return ok;
}

bool test_camera_start_reframe(const fs::path& root, int version) {
  auto before = fixture(root);
  auto camera = value(read_stitch_camera_selection(before));
  camera.input_start_times["right.mp4"] = 102;
  if (version >= 16) {
    camera.input_cameras["left.mp4"] = {109, 64, camera.input_lens};
    camera.input_cameras["right.mp4"] = {115, 70, camera.input_lens};
  }
  write_stitch_camera_selection(before, camera);
  if (version == 17)
    before["stitching"]["manual_control_points"] = std::string(64, 'a');
  before["hstream_ui"]["stitching_calibration"].remove("backend_generation");
  require(reserve_stitching_backend_generation_in_config(before, "completed-solve", choices(before)));
  provenance(root);
  if (version >= 16) {
    std::ifstream input(saved_state(root) / kStitchCanvasProvenanceArtifact);
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    text.replace(0, std::string("version=10").size(), "version=" + std::to_string(version));
    text += "input-lens=" + SerializeStitchInputLens(camera.input_lens) + "\n";
    if (version == 17)
      text += "manual-control-points=" + std::string(64, 'a') + "\n";
    text += "input-cameras=" + SerializeStitchInputCameras(camera.input_cameras) + "\ncontrol-point-filtering=0,0\n";
    write(saved_state(root) / kStitchCanvasProvenanceArtifact, text);
  }
  auto artifact_lock = value(lock_canvas_constraint_artifacts(root));
  auto config_lock = value(GameConfigTransactionLock::Acquire(root));
  const auto generation = value(stitch_artifact_generation_id_locked(root));
  auto desired = desired_view(before);
  auto prepared = PrepareStitchingReframeIntentLocked(root, before, desired, "level-with-start", desired);
  bool ok = expect(
      prepared.ok() && *prepared, "unchanged source starts permit leveling with provenance " + std::to_string(version));
  if (!prepared.ok())
    std::cerr << prepared.status() << '\n';
  if (!prepared.ok() || !*prepared)
    return false;
  pending(desired, "level-with-start");
  auto valid = ValidateStitchingReframeIntentLocked(root, YAML::Load(YAML::Dump(desired)));
  ok &= expect(valid.ok() && valid->source_generation == generation, "serialized leveling intent preserves alignment");
  for (bool edit_start : {true, false}) {
    auto changed = desired_view(before);
    if (edit_start)
      changed["stitching"]["input_start_times"]["right.mp4"] = 103;
    else if (version >= 16)
      changed["stitching"]["input_cameras"]["right.mp4"]["horizontal_fov"] = 116;
    else
      changed["stitching"]["camera_fov"]["horizontal_fov"] = 110;
    bool inputs_changed = false;
    auto result =
        PrepareStitchingReframeIntentLocked(root, before, changed, "changed-inputs", changed, &inputs_changed);
    ok &= expect(
        result.ok() && !*result && inputs_changed && !HasStitchingReframeIntent(changed),
        "changed starts or optics still require a fresh alignment");
    auto stale_claim = desired_view(changed);
    stale_claim["stitching"]["projection_framing"]["rotation_degrees"][1] = -25;
    ok &= expect(
        !PrepareStitchingReframeIntentLocked(root, changed, stale_claim, "stale-claim", stale_claim).ok(),
        "an old completed generation cannot authorize changed starts or optics");
    auto pending_change = YAML::Clone(desired);
    pending_change["stitching"] = YAML::Clone(changed["stitching"]);
    ok &= expect(
        !ValidateStitchingReframeIntentLocked(root, pending_change).ok(),
        "pending leveling rejects later changes to starts or optics");
  }
  return ok;
}

bool test_auto_playlist_materialization(const fs::path& root, bool legacy, bool camera_directories) {
  auto before = fixture(root);
  const std::vector<std::string> left = camera_directories
      ? std::vector<std::string>{"cam2/GX010106.MP4", "cam2/GX020106.MP4"}
      : std::vector<std::string>{"GX010106.MP4", "GX020106.MP4"};
  const std::vector<std::string> right = camera_directories
      ? std::vector<std::string>{"cam1/GX010105.MP4", "cam1/GX020105.MP4", "cam1/GX030105.MP4"}
      : std::vector<std::string>{"GX010105.MP4", "GX020105.MP4", "GX030105.MP4"};
  for (const auto& playlist : {left, right})
    for (const auto& path : playlist) {
      fs::create_directories((root / path).parent_path());
      write(root / path, "media identity fixture");
    }
  provenance(root);
  if (legacy) {
    auto camera = value(read_stitch_camera_selection(before));
    camera.input_cameras[left.front()] = {109, 64, camera.input_lens};
    camera.input_cameras[right.front()] = {115, 70, camera.input_lens};
    write_stitch_camera_selection(before, camera);
    before["hstream_ui"]["stitching_calibration"].remove("backend_generation");
    require(reserve_stitching_backend_generation_in_config(before, "completed-solve", choices(before)));
    std::ifstream input(saved_state(root) / kStitchCanvasProvenanceArtifact);
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    text.replace(0, std::string("version=10").size(), "version=16");
    text += "input-lens=" + SerializeStitchInputLens(camera.input_lens) + "\n";
    text += "input-cameras=" + SerializeStitchInputCameras(camera.input_cameras) + "\ncontrol-point-filtering=0,0\n";
    write(saved_state(root) / kStitchCanvasProvenanceArtifact, text);
  }
  before["game"]["videos"]["left"] = YAML::Node();
  before["game"]["videos"]["right"] = YAML::Node();
  auto artifact_lock = value(lock_canvas_constraint_artifacts(root));
  auto config_lock = value(GameConfigTransactionLock::Acquire(root));
  auto launch = desired_view(before);
  const auto prepared = PrepareStitchingReframeIntentLocked(root, before, launch, "auto-view", launch);
  bool ok = expect(prepared.ok() && *prepared, "Auto request captures the completed alignment");
  if (!prepared.ok() || !*prepared)
    return false;
  auto record = launch["hstream_ui"]["stitching_calibration"]["reframe"];
  ok &= expect(record["source_auto_videos"].size() == 2, "new Auto requests freeze both complete camera sets");
  if (legacy)
    record.remove("source_auto_videos");
  pending(launch, "auto-view");
  ok &= expect(ValidateStitchingReframeIntentLocked(root, launch).ok(), "Auto request validates before discovery");
  launch["game"]["videos"]["left"] = left;
  launch["game"]["videos"]["right"] = right;
  launch = YAML::Load(YAML::Dump(launch));
  const auto valid = ValidateStitchingReframeIntentLocked(root, launch);
  ok &= expect(valid.ok(), "Auto discovery can publish unequal complete playlists without invalidating reframe");
  if (!valid.ok())
    std::cerr << valid.status() << '\n';
  if (!legacy) {
    auto unbound_legacy = YAML::Clone(launch);
    unbound_legacy["hstream_ui"]["stitching_calibration"]["reframe"].remove("source_auto_videos");
    ok &= expect(
        !ValidateStitchingReframeIntentLocked(root, unbound_legacy).ok(),
        "legacy recovery requires saved independent camera identities");
  } else {
    auto replaced = YAML::Clone(launch);
    std::vector<std::string> other_camera;
    for (const auto& path : right) {
      const auto original = fs::path(path);
      auto filename = original.filename().string();
      filename.replace(4, 4, "0999");
      const auto other = (original.parent_path() / filename).generic_string();
      fs::rename(root / path, root / other);
      other_camera.push_back(other);
    }
    replaced["game"]["videos"]["right"] = other_camera;
    ok &= expect(
        !ValidateStitchingReframeIntentLocked(root, replaced).ok(),
        "legacy recovery rejects another complete discovered camera with unchanged saved optics");
    for (size_t i = 0; i < right.size(); ++i)
      fs::rename(root / other_camera[i], root / right[i]);
  }
  auto retry = YAML::Clone(launch);
  ok &= expect(
      value(PrepareResolvedStitchingReframeIntentLocked(root, launch, retry, "auto-retry", retry)),
      "a failed launch can rebind its saved Auto request after discovery");
  for (const char* change : {"reorder", "truncate", "duplicate", "replace", "role", "offset", "start", "optics"}) {
    auto changed = YAML::Clone(launch);
    const std::string name(change);
    if (name == "reorder")
      changed["game"]["videos"]["right"] = std::vector<std::string>{right[0], right[2], right[1]};
    else if (name == "truncate")
      changed["game"]["videos"]["right"] = std::vector<std::string>{right[0]};
    else if (name == "duplicate")
      changed["game"]["videos"]["right"] = left;
    else if (name == "replace")
      changed["game"]["videos"]["right"][0] = "GX010999.MP4";
    else if (name == "role")
      changed["hstream_ui"]["video_roles"]["left"] = left;
    else if (name == "offset")
      changed["game"]["stitching"]["frame_offsets"]["left"] = 8;
    else if (name == "start")
      changed["stitching"]["input_start_times"][right[0]] = 102;
    else
      changed["stitching"]["camera_fov"]["horizontal_fov"] = 120;
    ok &= expect(!ValidateStitchingReframeIntentLocked(root, changed).ok(), "Auto reframe rejects changed " + name);
  }
  // Bound source lists retain exact side/order identity, even when discovery
  // would also find those same files.
  auto explicit_before = YAML::Clone(before);
  explicit_before["game"]["videos"]["left"] = left;
  explicit_before["game"]["videos"]["right"] = right;
  auto explicit_launch = desired_view(explicit_before);
  ok &= expect(
      value(PrepareStitchingReframeIntentLocked(root, explicit_before, explicit_launch, "bound-view", explicit_launch)),
      "bound request captures");
  pending(explicit_launch, "bound-view");
  explicit_launch["game"]["videos"]["left"] = right;
  explicit_launch["game"]["videos"]["right"] = left;
  ok &=
      expect(!ValidateStitchingReframeIntentLocked(root, explicit_launch).ok(), "bound request rejects swapped sides");
  if (!legacy) {
    const auto extra = camera_directories ? "cam1/GX040105.MP4" : "GX040105.MP4";
    write(root / extra, "new chapter");
    launch["game"]["videos"]["right"].push_back(extra);
    ok &= expect(
        !ValidateStitchingReframeIntentLocked(root, launch).ok(), "new requests reject added discovered chapters");
    fs::remove(root / extra);
    fs::remove(root / right.back());
    launch["game"]["videos"]["right"] = std::vector<std::string>{right[0], right[1]};
    ok &= expect(
        !ValidateStitchingReframeIntentLocked(root, launch).ok(), "new requests reject removed discovered chapters");
  }
  return ok;
}

bool test_ownership_and_edits(const fs::path& root) {
  auto before = fixture(root);
  provenance(root);
  auto artifact_lock = value(lock_canvas_constraint_artifacts(root));
  auto config_lock = value(GameConfigTransactionLock::Acquire(root));
  bool reviews_ok = true;
  for (bool leveling : {false, true}) {
    for (bool crop : {false, true}) {
      auto promoted = YAML::Clone(before);
      promoted["hstream_ui"]["stitching_calibration"]["review_pending"] = true;
      promoted["hstream_ui"]["show_leveling_dialog"] = leveling;
      promoted["hstream_ui"]["show_crop_dialog"] = crop;
      auto desired_review = YAML::Clone(promoted);
      const auto plan = PrepareStitchingCalibrationControlsLocked(
          root,
          value(ReadStitchingCalibrationState(promoted, promoted)),
          value(ReadStitchingCalibrationControls(promoted)),
          promoted,
          promoted,
          "review-owner",
          false,
          false,
          false,
          false,
          desired_review);
      const bool requested = leveling || crop;
      reviews_ok &= expect(
          plan.ok() && plan->required == requested && plan->alignment_reused == requested &&
              plan->review_required == requested && plan->cleanup == StitchingCalibrationCleanup::kNone &&
              (!requested || ValidateStitchingReframeIntentLocked(root, desired_review).ok()),
          "Promoted Play must review enabled dialogs through retained alignment without matching or cleanup");
      if (!plan.ok())
        std::cerr << plan.status() << '\n';
    }
  }
  {
    auto fixed = YAML::Clone(before);
    fixed["hstream_ui"]["stitching_calibration"]["review_pending"] = true;
    fixed["stitching"]["projection_framing"]["auto_canvas"] = false;
    fixed["hstream_ui"]["stitching_calibration"].remove("backend_generation");
    require(reserve_stitching_backend_generation_in_config(fixed, "completed-solve", choices(fixed)));
    const auto provenance_path = saved_state(root) / kStitchCanvasProvenanceArtifact;
    std::ifstream provenance_input(provenance_path);
    const std::string saved_provenance(
        (std::istreambuf_iterator<char>(provenance_input)), std::istreambuf_iterator<char>());
    provenance_input.close();
    auto fixed_provenance = saved_provenance;
    const auto auto_canvas = fixed_provenance.find("projection-auto-canvas=1");
    fixed_provenance.replace(auto_canvas, std::string("projection-auto-canvas=1").size(), "projection-auto-canvas=0");
    write(provenance_path, fixed_provenance);
    auto fixed_review = YAML::Clone(fixed);
    const auto fixed_plan = PrepareStitchingCalibrationControlsLocked(
        root,
        value(ReadStitchingCalibrationState(fixed, fixed)),
        value(ReadStitchingCalibrationControls(fixed)),
        fixed,
        fixed,
        "fixed-review",
        false,
        false,
        false,
        false,
        fixed_review);
    reviews_ok &= expect(
        fixed_plan.ok() && fixed_plan->review_required && fixed_plan->cleanup == StitchingCalibrationCleanup::kNone &&
            ValidateStitchingReframeIntentLocked(root, fixed_review).ok(),
        "An unchanged fixed canvas can be reviewed without requiring a new alignment or view");
    if (!fixed_plan.ok())
      std::cerr << fixed_plan.status() << '\n';
    write(provenance_path, saved_provenance);
  }
  auto desired = desired_view(before);
  auto prepare = PrepareStitchingReframeIntentLocked(root, before, desired, "view-1", desired);
  bool ok = reviews_ok && expect(prepare.ok() && *prepare, "geometry edit captures the original complete alignment");
  if (!prepare.ok()) {
    std::cerr << prepare.status() << '\n';
    return false;
  }
  ok &= expect(
      !desired["hstream_ui"]["stitching_calibration"]["reframe"]["source_solver"]["manual_control_points"],
      "automatic reframe solver snapshots retain the legacy schema and fingerprints");
  auto changed_lens = desired_view(before);
  changed_lens["stitching"]["input_lens"]["projection"] = "full-frame-fisheye";
  bool solve_inputs_changed = false;
  const auto lens_reframe =
      PrepareStitchingReframeIntentLocked(root, before, changed_lens, "lens-edit", changed_lens, &solve_inputs_changed);
  ok &= expect(
      lens_reframe.ok() && !*lens_reframe && !HasStitchingReframeIntent(changed_lens) && solve_inputs_changed,
      "changing source optics requires alignment rather than reusing the old solve");
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
           {"manual_control_points", std::string(64, 'a')},
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
  write(saved_state(root) / "left.png", "replaced representative");
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
  fs::remove(saved_state(root) / kStitchCanvasProvenanceArtifact);
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

bool test_manual_identity(const fs::path& root) {
  auto before = fixture(root);
  const std::string manual(64, 'a');
  before["stitching"]["manual_control_points"] = manual;
  before["hstream_ui"]["stitching_calibration"].remove("backend_generation");
  require(reserve_stitching_backend_generation_in_config(before, "completed-solve", choices(before)));
  provenance(root);
  auto artifact_lock = value(lock_canvas_constraint_artifacts(root));
  auto config_lock = value(GameConfigTransactionLock::Acquire(root));
  auto desired = desired_view(before);
  bool ok = expect(
      !PrepareStitchingReframeIntentLocked(root, before, desired, "manual-view", desired).ok(),
      "manual inputs cannot reframe alignment carrying automatic provenance");
  std::ifstream input(saved_state(root) / kStitchCanvasProvenanceArtifact);
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  text.replace(0, std::string("version=10").size(), "version=11");
  text += "manual-control-points=" + manual + "\n";
  write(saved_state(root) / kStitchCanvasProvenanceArtifact, text);
  desired = desired_view(before);
  const auto prepared = PrepareStitchingReframeIntentLocked(root, before, desired, "manual-view", desired);
  ok &= expect(prepared.ok() && *prepared, "matching manual provenance authorizes a geometry-only reframe");
  if (!prepared.ok() || !*prepared)
    return false;
  pending(desired, "manual-view");
  ok &= expect(ValidateStitchingReframeIntentLocked(root, desired).ok(), "manual reframe snapshot validates unchanged");
  desired["stitching"]["manual_control_points"] = std::string(64, 'b');
  ok &= expect(
      !ValidateStitchingReframeIntentLocked(root, desired).ok(),
      "replacing manual matches invalidates a pending reframe");
  return ok;
}

bool test_selected_retention(const fs::path& root) {
  auto before = fixture(root);
  const auto plan = selected_plan(root);
  before["stitching"]["calibration_frame_selection"] = PlayerFrameSelectionPlanYaml(plan);
  before["stitching"]["calibration_frame_inputs_fingerprint"] = plan.fingerprint;
  before["hstream_ui"]["stitching_calibration"].remove("backend_generation");
  require(reserve_stitching_backend_generation_in_config(before, "completed-solve", choices(before)));
  std::vector<std::array<fs::path, 2>> images(2, {saved_state(root) / "left.png", saved_state(root) / "right.png"});
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

std::string read_bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// Exercise the actual native review protocol and commit callback. The Hugin
// unit tests supply their own build_config callback, bypassing that boundary.
class ReviewReplies : public std::stringbuf {
 public:
  ReviewReplies(const fs::path& game, bool leveling, bool crop, bool supersede, bool cancel)
      : game_(game),
        pending_(read_bytes(saved_state(game) / "config.yaml")),
        leveling_(leveling),
        crop_(crop),
        supersede_(supersede),
        cancel_(cancel),
        previous_(std::cout.rdbuf(this)) {}
  ~ReviewReplies() override {
    std::cout.rdbuf(previous_);
  }
  bool untouched{true};
  bool cancelled{false};
  int replies{0};
  std::string superseded_config;

 protected:
  int sync() override {
    const auto message = str();
    str("");
    previous_->sputn(message.data(), message.size());
    previous_->pubsync();
    const auto marker = message.find("status=ready directory-hex=");
    if (marker == std::string::npos)
      return 0;
    const auto start = marker + std::string("status=ready directory-hex=").size();
    const auto hex = message.substr(start, message.find_first_of(" \r\n", start) - start);
    std::string directory;
    for (size_t index = 0; index < hex.size(); index += 2)
      directory.push_back(static_cast<char>(std::stoul(hex.substr(index, 2), nullptr, 16)));
    const auto config_path = saved_state(game_) / "config.yaml";
    untouched &= read_bytes(config_path) == pending_;
    const bool leveling_prompt = message.find("HSTREAM_RINK_LEVELING") != std::string::npos;
    const fs::path response =
        fs::path(directory) / (leveling_prompt ? ".rink-leveling-response" : ".projection-crop-response");
    write(
        response.string() + ".tmp",
        leveling_prompt ? (leveling_ ? "use 25 0 0\n" : "skip\n")
                        : (crop_ ? "use 0 0.1 0.9 0.2 0.8\n" : "use 0 0 1 0 1\n"));
    fs::rename(response.string() + ".tmp", response);
    ++replies;
    if (!leveling_prompt && supersede_) {
      auto current = YAML::LoadFile(config_path.string());
      current["hstream_ui"]["stitching_calibration"]["backend_generation"]["control_point_matcher"] = "akaze-hamming";
      superseded_config = YAML::Dump(current) + "\n";
      write(config_path, superseded_config);
    }
    if (!leveling_prompt && cancel_)
      cancelled = true;
    return 0;
  }

 private:
  fs::path game_;
  std::string pending_;
  bool leveling_, crop_, supersede_, cancel_;
  std::streambuf* previous_;
};

bool test_review_publication(
    const fs::path& root,
    bool leveling,
    bool crop,
    bool inherited,
    bool supersede = false,
    bool cancel = false,
    bool reserved = true) {
  auto before = fixture(root);
  auto framing = value(read_stitch_projection_framing(before));
  framing.rotation_degrees = {0, 0, 0};
  framing.rotation_inherited = inherited;
  if (inherited) {
    framing.rotation_degrees[0] = 10;
    before["stitching"]["rink_configs"]["fixture"] =
        YAML::Load("{display_name: Fixture, rotation_degrees: [10, 0, 0]}");
    before["stitching"]["rink_config"] = "fixture";
  }
  write_stitch_projection_framing(before, framing);
  before["unrelated"]["keep"] = "retained";
  before["hstream_ui"]["stitching_calibration"].remove("backend_generation");
  require(reserve_stitching_backend_generation_in_config(before, "completed-solve", choices(before)));
  before["hstream_ui"]["stitching_calibration"]["review_pending"] = true;
  provenance(root);
  auto source = read_bytes(saved_state(root) / kStitchCanvasProvenanceArtifact);
  for (const auto& [old, replacement] : std::vector<std::pair<std::string, std::string>>{
           {"projection-rotation-0=0", inherited ? "projection-rotation-0=10" : "projection-rotation-0=0"},
           {"projection-rotation-1=-15", "projection-rotation-1=0"},
           {"projection-rotation-2=3", "projection-rotation-2=0"}})
    source.replace(source.find(old), old.size(), replacement);
  write(saved_state(root) / kStitchCanvasProvenanceArtifact, source);
  if (inherited) {
    for (const char* name : {"autooptimiser_out.pto", "hm_project.pto"}) {
      auto pto = read_bytes(saved_state(root) / name);
      pto.replace(pto.find("y0 "), 3, "y10 ");
      pto.replace(pto.find("y45 "), 4, "y55 ");
      write(saved_state(root) / name, pto);
    }
  }
  auto desired = YAML::Clone(before);
  {
    auto artifact_lock = value(lock_canvas_constraint_artifacts(root));
    auto config_lock = value(GameConfigTransactionLock::Acquire(root));
    if (!value(
            PrepareResolvedStitchingReframeIntentLocked(root, before, desired, "review-owner", desired, nullptr, true)))
      throw std::runtime_error("Review fixture did not retain alignment");
    pending(desired, "review-owner");
    if (reserved)
      require(reserve_stitching_backend_generation_in_config(desired, "review-owner", choices(desired)));
  }
  const auto pending_config = YAML::Dump(desired) + "\n";
  write(saved_state(root) / "config.yaml", pending_config);
  const auto old_project = read_bytes(saved_state(root) / "autooptimiser_out.pto");
  const auto old_map = read_bytes(saved_state(root) / "mapping_0000.tif");
  const auto tools = root / "tools";
  fs::create_directory(tools);
  const auto script = [&](const char* name, const std::string& body) {
    const auto path = tools / name;
    write(path, "#!/bin/sh\nset -eu\n" + body);
    fs::permissions(path, fs::perms::owner_all);
    return path.string();
  };
  const auto pano = script("pano", R"sh(
output=; input=; yaw=0; crop=0,100,0,100
while test "$#" -gt 0; do
  case "$1" in
    --output=*) output=${1#*=} ;;
    --rotate=*) rotation=${1#*=}; yaw=${rotation%%,*} ;;
    --crop=*) crop=${1#*=}; crop=${crop%%%} ;;
    --*) ;;
    *) input=$1 ;;
  esac
  shift
done
awk -v yaw="$yaw" -v crop="$crop" '
  /^p / { split(crop,c,","); printf "p f19 w64 h32 v180 P\"100 0 0\" S%d,%d,%d,%d\n", c[1]*.64,c[2]*.64,c[3]*.32,c[4]*.32; next }
  /^i / { for(i=1;i<=NF;i++) if($i ~ /^y[-+0-9.]+$/) $i="y" (substr($i,2)+yaw) }
  { print }
' "$input" > "$output"
)sh");
  cv::Mat x(32, 64, CV_16UC1), y(32, 64, CV_16UC1);
  for (int row = 0; row < x.rows; ++row)
    for (int column = 0; column < x.cols; ++column) {
      x.at<uint16_t>(row, column) = column % 16;
      y.at<uint16_t>(row, column) = row % 16;
    }
  cv::imwrite((tools / "x.tif").string(), x);
  cv::imwrite((tools / "y.tif").string(), y);
  cv::imwrite((tools / "rgba.tif").string(), cv::Mat(32, 64, CV_8UC4, cv::Scalar(1, 2, 3, 255)));
  cv::Mat seam(32, 64, CV_8UC1, cv::Scalar(0));
  seam.colRange(32, 64).setTo(255);
  cv::imwrite((tools / "seam.tif").string(), seam);
  for (const char* name : {"rgba.tif", "seam.tif"}) {
    TIFF* image = TIFFOpen((tools / name).c_str(), "r+");
    if (!image)
      throw std::runtime_error("Cannot open review TIFF fixture");
    TIFFSetField(image, TIFFTAG_XRESOLUTION, 1.0f);
    TIFFSetField(image, TIFFTAG_YRESOLUTION, 1.0f);
    TIFFSetField(image, TIFFTAG_XPOSITION, 0.0f);
    TIFFSetField(image, TIFFTAG_YPOSITION, 0.0f);
    const bool written = TIFFRewriteDirectory(image) != 0;
    TIFFClose(image);
    if (!written)
      throw std::runtime_error("Cannot add TIFF placement to review fixture");
  }
  cv::imwrite((tools / "panorama.tif").string(), cv::Mat(32, 64, CV_8UC3, cv::Scalar(4, 5, 6)));
  const auto nona = script(
      "nona",
      "for camera in 0000 0001; do\ncp '" + (tools / "rgba.tif").string() + "' mapping_$camera.tif\ncp '" +
          (tools / "x.tif").string() + "' mapping_${camera}_x.tif\ncp '" + (tools / "y.tif").string() +
          "' mapping_${camera}_y.tif\ndone\n");
  const auto enblend = script(
      "enblend",
      "cp '" + (tools / "seam.tif").string() + "' seam_file.tif\ncp '" + (tools / "panorama.tif").string() +
          "' panorama.tif\n");
  ScopedEnvironment pano_env("HM_PANO_MODIFY", pano.c_str()), nona_env("HM_NONA", nona.c_str()),
      enblend_env("HM_ENBLEND", enblend.c_str()), leveling_env("HSTREAM_RINK_LEVELING_FLOW", "1"),
      crop_env("HSTREAM_PROJECTION_CROP_FLOW", "1");
  ReviewReplies replies(root, leveling, crop, supersede, cancel);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  const auto status = reframe_stitching(
      root.string(),
      "review-owner",
      [&] { return replies.cancelled || std::chrono::steady_clock::now() >= deadline; },
      0);
  bool ok = expect(replies.replies == 2 && replies.untouched, "review choices stay private until publication");
  if (supersede || cancel) {
    ok &= expect(supersede ? absl::IsAborted(status) : absl::IsCancelled(status), "superseded/cancelled review fails");
    ok &= expect(
        read_bytes(saved_state(root) / "config.yaml") == (supersede ? replies.superseded_config : pending_config) &&
            read_bytes(saved_state(root) / "autooptimiser_out.pto") == old_project &&
            read_bytes(saved_state(root) / "mapping_0000.tif") == old_map,
        "failed review preserves saved config and original alignment/maps");
    return ok;
  }
  if (!status.ok()) {
    std::cerr << status << '\n';
    return false;
  }
  const auto committed = YAML::LoadFile((saved_state(root) / "config.yaml").string());
  const auto actual = value(read_stitch_projection_framing(committed));
  if (leveling) {
    framing.rotation_degrees = {25, 0, 0};
    framing.rotation_inherited = false;
  }
  if (crop)
    framing.crop = {0.1, 0.9, 0.2, 0.8};
  if (!leveling && !crop)
    ok &= expect(
        read_bytes(saved_state(root) / "autooptimiser_out.pto") == old_project &&
            read_bytes(saved_state(root) / "mapping_0000.tif") == old_map,
        "accepting the unchanged review retains the exact saved project and maps");
  ok &= expect(
      actual == framing && actual.rotation_inherited == framing.rotation_inherited &&
          validate_stitching_backend_generation(committed, "review-owner", choices(committed)).ok() &&
          committed["unrelated"]["keep"].as<std::string>() == "retained" &&
          committed["hstream_ui"]["stitching_calibration"]["status"].as<std::string>() == "complete" &&
          !committed["hstream_ui"]["stitching_calibration"]["review_pending"] && !HasStitchingReframeIntent(committed),
      "review commits the selected framing and exact claim together, preserving unrelated settings");
  auto artifacts = value(HuginProject::RecoverAndLock(root));
  const auto published = value(HuginProject::ReadCanvasProvenance(root, *artifacts));
  ok &= expect(
      published && published->projection_framing == framing,
      "published map provenance agrees with the accepted framing");
  return ok;
}

} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("stitching-reframe-test-" + std::to_string(::getpid()));
  try {
    ScopedEnvironment no_platform_limit("HM_ALLOW_OVERSIZED_LIVE_STITCH", "1");
    bool ok = test_optional_presence();
    ok &= test_serialized_intent(root / "serialized");
    for (int version : {10, 16, 17})
      ok &= test_camera_start_reframe(root / ("camera-start-" + std::to_string(version)), version);
    for (bool legacy : {false, true})
      for (bool camera_directories : {false, true})
        ok &= test_auto_playlist_materialization(
            root / (std::string(legacy ? "legacy-auto" : "auto") + (camera_directories ? "-dirs" : "-root")),
            legacy,
            camera_directories);
    ok &= test_ownership_and_edits(root / "ownership");
    ok &= test_capture_guards(root / "capture");
    ok &= test_environment_canvas_limit(root / "environment");
    ok &= test_completed_runtime_reservation(root / "runtime-reservation");
    ok &= test_projections_without_parameters(root / "no-parameters");
    ok &= test_selected_retention(root / "selected");
    ok &= test_manual_identity(root / "manual");
    for (bool leveling : {false, true})
      for (bool crop : {false, true})
        ok &= test_review_publication(
            root / ("review-" + std::to_string(leveling) + std::to_string(crop)), leveling, crop, false);
    ok &= test_review_publication(root / "review-inherited", false, true, true);
    ok &= test_review_publication(root / "review-inherited-unchanged", false, false, true);
    ok &= test_review_publication(root / "review-unreserved", true, true, false, false, false, false);
    ok &= test_review_publication(root / "review-unreserved-unchanged", false, false, true, false, false, false);
    ok &= test_review_publication(root / "review-superseded", true, true, false, true);
    ok &= test_review_publication(root / "review-cancelled", true, true, false, false, true);
    fs::remove_all(root);
    return ok ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    fs::remove_all(root);
    return 1;
  }
}
