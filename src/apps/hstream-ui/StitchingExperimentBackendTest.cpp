#include "src/apps/hstream-ui/StitchingExperimentBackend.h"

#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/PlayerFrameSelection.h"

#include <opencv2/imgcodecs.hpp>
#include <tiffio.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "yaml-cpp/yaml.h"

namespace fs = std::filesystem;

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

bool write(const fs::path& path, const std::string& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path);
  output << contents;
  return output.good();
}

bool inherited_camera_handoff(const fs::path& root) {
  using namespace hm::stitching;
  const fs::path game = root / "inherited-camera-game";
  if (!write(game / "cam1" / "left.mp4", "left") || !write(game / "cam2" / "right.mp4", "right") ||
      !write(game / "config.yaml", "game:\n  videos:\n    left: [cam1/left.mp4]\n    right: [cam2/right.mp4]\n"))
    return false;
  const StitchingExperimentSettings settings{900, 2, "00:00:00", std::nullopt};
  const auto baseline = CreateStitchingExperimentWorkspace(game, root / "inherited-camera", settings, 1);
  const auto candidate = CreateStitchingExperimentWorkspace(game, root / "inherited-camera", settings, 2);
  if (!baseline.ok() || !candidate.ok())
    return false;
  const fs::path directory = baseline->game_directory;
  for (const char* name : {"hm_project.pto", "autooptimiser_out.pto"})
    if (!write(directory / name, "fixture"))
      return false;
  for (const char* name :
       {"mapping_0000.tif",
        "mapping_0000_x.tif",
        "mapping_0000_y.tif",
        "mapping_0001.tif",
        "mapping_0001_x.tif",
        "mapping_0001_y.tif"})
    if (!cv::imwrite((directory / name).string(), cv::Mat(8, 8, CV_16UC1, cv::Scalar(1))))
      return false;
  for (const char* name : {"mapping_0000.tif", "mapping_0001.tif"}) {
    TIFF* tiff = TIFFOpen((directory / name).c_str(), "r+");
    if (!tiff)
      return false;
    TIFFSetField(tiff, TIFFTAG_XRESOLUTION, 1.0f);
    TIFFSetField(tiff, TIFFTAG_YRESOLUTION, 1.0f);
    TIFFSetField(tiff, TIFFTAG_XPOSITION, 0.0f);
    TIFFSetField(tiff, TIFFTAG_YPOSITION, 0.0f);
    const bool written = TIFFRewriteDirectory(tiff);
    TIFFClose(tiff);
    if (!written)
      return false;
  }
  const cv::Mat mask(8, 8, CV_8UC1, cv::Scalar(255));
  if (!cv::imwrite((directory / "seam_file.png").string(), mask) ||
      !cv::imwrite((directory / "rink_mask_0.png").string(), mask))
    return false;
  std::string generation;
  {
    auto lock = HuginProject::RecoverAndLock(directory);
    if (!lock.ok())
      return false;
    auto identity = HuginProject::GenerationId(directory, **lock);
    if (!identity.ok())
      return false;
    generation = *identity;
  }
  const auto output_generation = stitched_output_generation_id(generation, 0);
  const auto mask_fingerprint = PlayerFrameMaskFingerprint(mask);
  if (!output_generation.ok() || !mask_fingerprint.ok())
    return false;
  YAML::Node resolved = YAML::LoadFile((directory / "config.yaml").string());
  // Mimic the baseline runner materializing a user-level camera/FOV override;
  // the sibling candidate still has only the original game's explicit values.
  const StitchCameraSelection camera{"custom-user-camera", 112.5, 88.0};
  write_stitch_camera_selection(resolved, camera);
  resolved["game"]["stitching"]["frame_offsets"]["left"] = 0;
  resolved["game"]["stitching"]["frame_offsets"]["right"] = 1;
  resolved["hstream_ui"]["stitching_calibration"]["status"] = "complete";
  resolved["rink"]["stitched_output_generation"] = *output_generation;
  if (!write(directory / "config.yaml", YAML::Dump(resolved)))
    return false;
  const auto context = player_frame_source_context(resolved, 0);
  if (!context.ok())
    return false;
  PlayerFrameSelectionPlan plan;
  plan.settings.frame_count = 2;
  plan.context = {
      {"source_context", *context},
      {"baseline_generation", generation},
      {"output_generation", *output_generation},
      {"detector_identity", "fixture"},
      {"rink_mask_sha256", *mask_fingerprint},
      {"rink_mask_revision", "fixture"},
      {"fieldmask_settings", "fixture"},
      {"output_rotation_degrees", "0"},
      {"decode_anchor_ns", "0"}};
  for (const fs::path& path : {game / "cam1" / "left.mp4", game / "cam2" / "right.mp4"}) {
    const auto source = BindPlayerFrameSource(path);
    if (!source.ok())
      return false;
    plan.sources.push_back(*source);
  }
  for (uint64_t index = 0; index < 2; ++index) {
    PlayerFrameObservation frame;
    frame.pair.timeline_pts_ns = index * kPlayerFrameSecond;
    for (size_t camera_index = 0; camera_index < 2; ++camera_index)
      frame.pair.cameras[camera_index] = {plan.sources[camera_index].path, index * kPlayerFrameSecond};
    frame.coverage = {5};
    frame.eligible_people = 1;
    frame.size_band_counts = {1, 0, 0};
    frame.quality = 1;
    plan.selected.push_back(frame);
  }
  const auto fingerprint = PlayerFrameSelectionFingerprint(plan);
  if (!fingerprint.ok())
    return false;
  plan.fingerprint = *fingerprint;
  PlayerFrameSelectionReport report;
  report.observation_count = 2;
  report.observed_size_band_counts = {2, 0, 0};
  report.plan = plan;
  const fs::path report_path = root / "inherited-camera-report.yaml";
  if (!write(report_path, YAML::Dump(PlayerFrameSelectionReportYaml(report))))
    return false;
  const auto prepared = PreparePlayerSelectedStitchingExperiment(*baseline, *candidate, report_path);
  if (!prepared.ok()) {
    std::cerr << prepared.status() << '\n';
    return false;
  }
  const YAML::Node frozen = YAML::LoadFile((candidate->game_directory / "config.yaml").string());
  const auto frozen_camera = read_stitch_camera_selection(frozen);
  return prepared->available && frozen_camera.ok() && *frozen_camera == camera &&
      validate_player_frame_selection_sources(frozen).ok();
}

} // namespace

int main() {
  char root_template[] = "/tmp/hstream-stitch-experiment-backend-XXXXXX";
  const char* created = ::mkdtemp(root_template);
  if (!created)
    return 1;
  const fs::path root(created);
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(root, ignored);
    }
  } cleanup{root};

  const fs::path game = root / "game";
  const fs::path experiments = root / "experiments";
  if (!write(game / "cam1" / "left.mp4", "left") || !write(game / "cam2" / "right.mp4", "right") ||
      !write(game / "program-output.mp4", "not a camera input") || !write(game / "left_calibration.json", "{}") ||
      !write(game / "cam2" / "right-calibration.yaml", "camera: right") ||
      !write(
          game / "config.yaml",
          "game:\n"
          "  videos:\n"
          "    left: [cam1/left.mp4]\n"
          "    right: [cam2/right.mp4]\n"
          "  stitching:\n"
          "    control_points: [old-game-cache]\n"
          "stitching:\n"
          "  control_points: [old-cache]\n"
          "  calibration_frame_selection: {fingerprint: old-player-plan}\n"
          "  mapping_backend: nona\n"
          "  projection: general-panini\n"
          "  projection_framing:\n"
          "    rotation_degrees: [7, -2, 1]\n"
          "rink:\n"
          "  scoreboard:\n"
          "    perspective_polygon: [[1, 2], [3, 4]]\n"
          "  ice_contours_mask_count: 2\n"
          "  ice_contours_mask_centroid: [10, 20]\n"
          "  ice_contours_combined_bbox: [0, 0, 100, 50]\n"
          "  stitched_output_pending_previous_generation: old\n"
          "  stitched_output_pending_previous_authorization_id: auth\n"
          "  stitched_output_pending_previous_owner_process: 123\n"
          "  stitched_output_pending_completed_scoreboard_polygon: [[5, 6], [7, 8]]\n")) {
    return 2;
  }

  StitchingExperimentSettings settings{
      .control_points = 900,
      .frame_count = 4,
      .stitch_frame_time = "00:01:02.500",
      .rink_rotation_degrees = std::array<double, 3>{0.0, -1.5, 0.5},
  };
  const auto workspace = CreateStitchingExperimentWorkspace(game, experiments, settings, 1);
  if (!expect(workspace.ok(), "candidate workspace must be created")) {
    if (!workspace.ok())
      std::cerr << workspace.status() << '\n';
    return 3;
  }

  bool ok = true;
  ok &= expect(inherited_camera_handoff(root), "candidate handoff must freeze inherited baseline camera and FOV");
  ok &= expect(fs::is_symlink(workspace->game_directory / "cam1" / "left.mp4"), "left video must be linked");
  ok &= expect(fs::is_symlink(workspace->game_directory / "cam2" / "right.mp4"), "right video must be linked");
  ok &= expect(
      !fs::exists(workspace->game_directory / "program-output.mp4"),
      "unconfigured Program output must not become an experiment camera input");
  ok &= expect(
      fs::is_symlink(workspace->game_directory / "left_calibration.json"), "root camera calibration must be linked");
  ok &= expect(
      fs::is_symlink(workspace->game_directory / "cam2" / "right-calibration.yaml"),
      "camera-directory calibration must be linked");

  const fs::path outside = root / "outside.mp4";
  if (!write(outside, "outside"))
    return 4;
  std::error_code symlink_error;
  fs::create_symlink(outside, game / "cam1" / "escaped.mp4", symlink_error);
  const auto escaped_workspace = CreateStitchingExperimentWorkspace(game, experiments, settings, 2);
  ok &= expect(
      !escaped_workspace.ok() && absl::IsInvalidArgument(escaped_workspace.status()),
      "auto-discovered video symlinks must not escape the selected game");

  const YAML::Node config = YAML::LoadFile((workspace->game_directory / "config.yaml").string());
  const YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
  const YAML::Node rotation = config["stitching"]["projection_framing"]["rotation_degrees"];
  ok &= expect(calibration["control_points"].as<int>() == 900, "candidate control-point count must be saved");
  ok &= expect(calibration["frame_count"].as<int>() == 4, "candidate frame count must be saved");
  ok &= expect(calibration["status"].as<std::string>() == "pending", "candidate calibration must be pending");
  ok &= expect(
      !config["stitching"]["calibration_frame_selection"].IsDefined(),
      "ordinary workspaces must clear inherited player frame selection");
  ok &= expect(
      config["stitching"]["stitch_frame_time"].as<std::string>() == "00:01:02.500",
      "candidate first frame must be saved");
  ok &= expect(
      rotation.size() == 3 && rotation[0].as<double>() == 7.0 && rotation[1].as<double>() == -1.5 &&
          rotation[2].as<double>() == 0.5,
      "candidate rink pitch and roll must override the saved setting while preserving yaw");
  ok &= expect(
      calibration["invalidation_id"].as<std::string>() == workspace->invalidation_id &&
          !calibration["backend_generation"].IsDefined(),
      "candidate must leave backend reservation to the runner after inherited settings resolve");
  const auto selected_config =
      BuildStitchingExperimentSelectionConfig(workspace->game_directory / "config.yaml", game / "config.yaml");
  ok &= expect(selected_config.ok(), "selected candidate config must be mergeable");
  if (selected_config.ok()) {
    const YAML::Node selected = YAML::Load(*selected_config);
    const YAML::Node selected_rink = selected["rink"];
    ok &= expect(
        !selected["stitching"]["calibration_frame_selection"].IsDefined(),
        "promoting an ordinary candidate must remove the previous player plan");
    ok &= expect(
        !selected["stitching"]["control_points"].IsDefined() &&
            !selected["game"]["stitching"]["control_points"].IsDefined() &&
            !selected_rink["scoreboard"]["perspective_polygon"].IsDefined() &&
            !selected_rink["ice_contours_mask_count"].IsDefined() &&
            !selected_rink["ice_contours_mask_centroid"].IsDefined() &&
            !selected_rink["ice_contours_combined_bbox"].IsDefined() &&
            !selected_rink["stitched_output_pending_previous_generation"].IsDefined() &&
            !selected_rink["stitched_output_pending_previous_authorization_id"].IsDefined() &&
            !selected_rink["stitched_output_pending_previous_owner_process"].IsDefined() &&
            !selected_rink["stitched_output_pending_completed_scoreboard_polygon"].IsDefined(),
        "selection must remove every geometry-dependent rink cache and pending recovery field");
  }

  const fs::path report = root / "player-report.yaml";
  const auto missing_report = PreparePlayerSelectedStitchingExperiment(*workspace, *workspace, report);
  ok &= expect(!missing_report.ok(), "a missing player report must be an error");
  if (!write(report, "schema_version: 999\nobservation_count: 0\n"))
    return 6;
  const auto unknown_report = PreparePlayerSelectedStitchingExperiment(*workspace, *workspace, report);
  ok &= expect(!unknown_report.ok(), "unknown player report versions must not become ordinary fallback candidates");
  ok &= expect(
      YAML::Dump(YAML::LoadFile((workspace->game_directory / "config.yaml").string())) == YAML::Dump(config),
      "invalid reports must leave the pending candidate unchanged");
  hm::stitching::PlayerFrameSelectionReport empty_scan;
  empty_scan.observation_count = 4;
  empty_scan.unavailable_reason = "No separated on-ice people in the searched passage";
  if (!write(report, YAML::Dump(hm::stitching::PlayerFrameSelectionReportYaml(empty_scan))))
    return 7;
  const auto unavailable = PreparePlayerSelectedStitchingExperiment(*workspace, *workspace, report);
  ok &= expect(
      unavailable.ok() && !unavailable->available && unavailable->diagnostic == empty_scan.unavailable_reason,
      "clean insufficient coverage must retain its diagnostic as a quality outcome");
  ok &= expect(
      YAML::Dump(YAML::LoadFile((workspace->game_directory / "config.yaml").string())) == YAML::Dump(config),
      "insufficient coverage must not freeze a plan or change pending calibration");

  YAML::Node selected_with_plan = YAML::Clone(config);
  hm::stitching::PlayerFrameSelectionPlan plan;
  plan.context = {
      {"source_context", "fixture"},
      {"baseline_generation", "fixture"},
      {"output_generation", "fixture"},
      {"detector_identity", "fixture"},
      {"rink_mask_sha256", "fixture"},
      {"rink_mask_revision", "fixture"},
      {"fieldmask_settings", "fixture"},
      {"output_rotation_degrees", "0"},
      {"decode_anchor_ns", "62500000000"}};
  for (const fs::path& camera : {game / "cam1" / "left.mp4", game / "cam2" / "right.mp4"}) {
    auto source = hm::stitching::BindPlayerFrameSource(camera);
    if (!source.ok())
      return 8;
    plan.sources.push_back(*source);
  }
  for (uint64_t index = 0; index < 4; ++index) {
    hm::stitching::PlayerFrameObservation frame;
    frame.pair.timeline_pts_ns = index * hm::stitching::kPlayerFrameSecond;
    for (size_t camera = 0; camera < 2; ++camera)
      frame.pair.cameras[camera] = {
          plan.sources[camera].path, frame.pair.timeline_pts_ns, static_cast<uint32_t>(camera), index * 30};
    frame.coverage = {5, 388};
    frame.eligible_people = 2;
    frame.size_band_counts = {1, 0, 1};
    frame.quality = 1.25;
    plan.selected.push_back(frame);
  }
  auto fingerprint = hm::stitching::PlayerFrameSelectionFingerprint(plan);
  if (!fingerprint.ok())
    return 9;
  plan.fingerprint = *fingerprint;
  selected_with_plan["stitching"]["calibration_frame_selection"] = hm::stitching::PlayerFrameSelectionPlanYaml(plan);
  if (!write(workspace->game_directory / "config.yaml", YAML::Dump(selected_with_plan)))
    return 7;
  const auto selected_plan_config =
      BuildStitchingExperimentSelectionConfig(workspace->game_directory / "config.yaml", game / "config.yaml");
  ok &= expect(selected_plan_config.ok(), "selection provenance must be included in the promotion merge");
  if (selected_plan_config.ok())
    ok &= expect(
        YAML::Load(*selected_plan_config)["stitching"]["calibration_frame_selection"]["fingerprint"]
                .as<std::string>() == plan.fingerprint,
        "selected inline player provenance must replace the old game's plan");
  const auto inspected = InspectStitchingExperimentFrames(*workspace);
  ok &= expect(inspected.ok(), "frozen frame choices must remain inspectable before a successful solve");
  if (inspected.ok()) {
    ok &= expect(
        inspected->frames.size() == 4 && inspected->anchor_ns == 62500000000ULL &&
            inspected->frames[1].source_pts_ns[0] == hm::stitching::kPlayerFrameSecond &&
            inspected->frames[1].decoded_sequences[1] == 30 &&
            inspected->frames[1].coverage == plan.selected[1].coverage,
        "inspection must preserve exact raw identities and scoring coverage");
    ok &= expect(
        inspected->frames[1].thumbnails[0] ==
            workspace->game_directory / "player-frame-inspection" / plan.fingerprint / "left_1.jpg",
        "inspection thumbnail paths must belong to the selected fingerprint");
  }
  if (!write(game / "cam2" / "right.mp4", "changed-source"))
    return 10;
  const auto changed_source = InspectStitchingExperimentFrames(*workspace);
  ok &= expect(
      changed_source.ok() && changed_source->source_validation.find("Source validation:") == 0,
      "inspection must expose changed sources while retaining the recorded choices");
  selected_with_plan["hstream_ui"]["stitching_calibration"]["status"] = "failed";
  if (!write(workspace->game_directory / "config.yaml", YAML::Dump(selected_with_plan)))
    return 11;
  ok &=
      expect(InspectStitchingExperimentFrames(*workspace).ok(), "failed solves must retain inspectable frozen choices");
  selected_with_plan["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "superseding-generation";
  if (!write(workspace->game_directory / "config.yaml", YAML::Dump(selected_with_plan)))
    return 12;
  const auto superseded = InspectStitchingExperimentFrames(*workspace);
  ok &= expect(
      !superseded.ok() && absl::IsAborted(superseded.status()),
      "inspection must reject a workspace replaced by another generation");
  return ok ? 0 : 5;
}
