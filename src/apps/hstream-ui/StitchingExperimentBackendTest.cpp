#include "src/apps/hstream-ui/StitchingExperimentBackend.h"
#include "src/apps/hstream-ui/StitchingExperimentStore.h"

#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/PlayerFrameSelection.h"

#include <opencv2/imgcodecs.hpp>
#include <sys/syscall.h>
#include <tiffio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "yaml-cpp/yaml.h"

namespace fs = std::filesystem;

namespace {

std::mutex fsync_probe_mutex;
bool fsync_probe_active = false;
fs::path fsync_failure_path;
int fsync_failures = 0;
std::vector<fs::path> fsync_paths;

} // namespace

// Interpose only in this test executable so the production durability barriers
// experience a real EIO return without adding production failure switches.
extern "C" int fsync(int descriptor) {
  {
    std::lock_guard<std::mutex> lock(fsync_probe_mutex);
    if (fsync_probe_active) {
      char target[4096];
      const auto count = ::readlink(("/proc/self/fd/" + std::to_string(descriptor)).c_str(), target, sizeof(target));
      if (count > 0 && count < static_cast<ssize_t>(sizeof(target))) {
        const fs::path path(std::string(target, static_cast<size_t>(count)));
        fsync_paths.push_back(path);
        if (path == fsync_failure_path && fsync_failures == 0) {
          ++fsync_failures;
          errno = EIO;
          return -1;
        }
      }
    }
  }
  return static_cast<int>(::syscall(SYS_fsync, descriptor));
}

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

struct FsyncProbe {
  explicit FsyncProbe(const fs::path& fail_at = {}) {
    std::lock_guard<std::mutex> lock(fsync_probe_mutex);
    fsync_failure_path = fail_at;
    fsync_failures = 0;
    fsync_paths.clear();
    fsync_probe_active = true;
  }
  ~FsyncProbe() {
    std::lock_guard<std::mutex> lock(fsync_probe_mutex);
    fsync_probe_active = false;
  }
};

bool durable_workspace_publication(const fs::path& root) {
  const fs::path game = root / "durable-game";
  const fs::path left = game / "cam1" / "chapters" / "left.mp4";
  const fs::path right = game / "cam2" / "right.mp4";
  const fs::path sidecar = game / "cam1" / "chapters" / "left-calibration.yaml";
  if (!write(left, "left recording") || !write(right, "right recording") || !write(sidecar, "camera: left") ||
      !write(
          game / "config.yaml", "game:\n  videos:\n    left: [cam1/chapters/left.mp4]\n    right: [cam2/right.mp4]\n"))
    return false;
  auto store = OpenStitchingExperimentStore(game);
  if (!expect(store.ok(), "durable workspace fixture store must open"))
    return false;
  const StitchingExperimentSettings settings{100, 2, "00:00:00", std::nullopt};
  const auto create_queued = [&](const fs::path& session, int sequence) -> absl::Status {
    auto workspace = CreateStitchingExperimentWorkspace(game, session, settings, sequence);
    if (!workspace.ok())
      return workspace.status();
    StoredStitchingExperiment record;
    record.workspace = *workspace;
    record.state = "queued";
    record.sequence = sequence;
    return SaveStitchingExperiment(*store, record);
  };
  bool ok = true;
  for (int stage = 0; stage < 6; ++stage) {
    const fs::path session = store->directory / "sessions" / ("failed-session-" + std::to_string(stage));
    const fs::path candidate = session / ("durable-game-stitch-exp-" + std::to_string(stage + 1));
    const std::vector<fs::path> barriers{
        candidate / "config.yaml",
        candidate / "cam1" / "chapters",
        candidate,
        session,
        session.parent_path(),
        store->directory};
    absl::Status created;
    {
      FsyncProbe probe(barriers[stage]);
      created = create_queued(session, stage + 1);
      ok &= expect(fsync_failures == 1, "the selected config or directory fsync failure must be exercised");
      ok &= expect(
          std::find(fsync_paths.begin(), fsync_paths.end(), left) == fsync_paths.end() &&
              std::find(fsync_paths.begin(), fsync_paths.end(), right) == fsync_paths.end() &&
              std::find(fsync_paths.begin(), fsync_paths.end(), sidecar) == fsync_paths.end(),
          "workspace durability must never fsync linked source recordings or sidecars");
    }
    ok &= expect(!created.ok(), "a failed workspace durability barrier must abort queued publication");
    const auto catalog = LoadStitchingExperimentStore(*store);
    ok &= expect(
        catalog.ok() && catalog->experiments.empty(),
        "a config, media-directory, candidate, or session sync failure must never advertise a queued row");
    ok &= expect(!fs::exists(candidate), "failed initial workspace creation must remove only its private candidate");
    ok &= expect(
        fs::is_regular_file(left) && fs::is_regular_file(right) && fs::is_regular_file(sidecar),
        "failed workspace cleanup must preserve linked inputs");
  }
  const fs::path session = store->directory / "sessions" / "successful-session";
  const fs::path candidate = session / "durable-game-stitch-exp-10";
  {
    FsyncProbe probe;
    ok &= expect(create_queued(session, 10).ok(), "a completely synced workspace must become a queued row");
    const std::vector<fs::path> required{
        candidate / "config.yaml",
        candidate / "cam1" / "chapters",
        candidate / "cam1",
        candidate,
        session,
        session.parent_path(),
        store->directory};
    auto previous = fsync_paths.begin();
    for (const auto& path : required) {
      const auto found = std::find(previous, fsync_paths.end(), path);
      ok &= expect(
          found != fsync_paths.end(),
          "config and child-to-parent durability barriers must precede catalog publication");
      if (found == fsync_paths.end())
        break;
      previous = std::next(found);
    }
    ok &= expect(
        std::find_if(
            previous,
            fsync_paths.end(),
            [&](const fs::path& path) {
              return path.parent_path() == store->directory && path.filename().string().find(".write-") == 0;
            }) != fsync_paths.end(),
        "the catalog may be fsynced only after every workspace and session barrier");
  }
  const auto catalog = LoadStitchingExperimentStore(*store);
  ok &=
      expect(catalog.ok() && catalog->experiments.size() == 1, "reopening must retain exactly the durable queued row");
  const auto duplicate = CreateStitchingExperimentWorkspace(game, session, settings, 10);
  ok &= expect(
      !duplicate.ok() && absl::IsAlreadyExists(duplicate.status()), "initial workspace creation must remain exclusive");
  return ok;
}

bool ordinary_frame_inspection(const StitchingExperimentWorkspace& workspace) {
  const fs::path config_path = workspace.game_directory / "config.yaml";
  const YAML::Node original = YAML::LoadFile(config_path.string());
  const fs::path manifest_path =
      workspace.game_directory / "calibration-frame-inspection" / workspace.invalidation_id / "frames.yaml";
  YAML::Node manifest;
  manifest["version"] = 1;
  manifest["invalidation_id"] = workspace.invalidation_id;
  manifest["expected_pair_count"] = workspace.settings.frame_count;
  for (int index = 0; index < workspace.settings.frame_count; ++index) {
    YAML::Node pair;
    pair["index"] = index;
    pair["left"]["path"] = "left.mp4";
    pair["left"]["source_seconds"] = 62.5 + index;
    pair["right"]["path"] = "";
    pair["right"]["source_seconds"] = 0;
    manifest["pairs"].push_back(pair);
    if (index == 1) {
      if (!write(manifest_path, YAML::Dump(manifest)))
        return false;
      const auto partial = InspectStitchingExperimentFrames(workspace);
      if (!expect(
              partial.ok() && !partial->player_selected && partial->frames.size() == 2 &&
                  partial->source_validation.find("Partial capture") != std::string::npos &&
                  partial->frames.front().camera_paths[1].empty() && partial->frames.front().coverage.empty() &&
                  partial->frames[1].match_images[1] == manifest_path.parent_path() / "matches_1.jpg",
              "ordinary inspection must identify partial captures and never invent player scores or source identity"))
        return false;
      YAML::Node complete = YAML::Clone(original);
      complete["hstream_ui"]["stitching_calibration"]["status"] = "complete";
      if (!write(config_path, YAML::Dump(complete)) ||
          !expect(
              !InspectStitchingExperimentFrames(workspace).ok(),
              "a complete candidate must not present an incomplete inspection manifest"))
        return false;
    }
  }
  if (!write(manifest_path, YAML::Dump(manifest)))
    return false;
  const auto complete = InspectStitchingExperimentFrames(workspace);
  if (!expect(
          complete.ok() && !complete->player_selected && complete->frames.size() == 4 &&
              complete->frames[1].source_seconds[0] == 63.5 &&
              complete->frames[1].thumbnails[0] == manifest_path.parent_path() / "left_1.jpg",
          "ordinary inspection must read the highlighted candidate's captured source time and private stills"))
    return false;
  manifest["invalidation_id"] = "another-owner";
  if (!write(manifest_path, YAML::Dump(manifest)) ||
      !expect(!InspectStitchingExperimentFrames(workspace).ok(), "inspection must reject a stale capture owner"))
    return false;
  return write(config_path, YAML::Dump(original));
}

bool inherited_camera_handoff(const fs::path& root) {
  using namespace hm::stitching;
  const fs::path game = root / "inherited-camera-game";
  YAML::Node original;
  for (const auto& section : {std::make_pair("game", "videos"), std::make_pair("hstream_ui", "video_roles")}) {
    original[section.first][section.second]["left"].push_back((game / "cam1" / "left.mp4").string());
    original[section.first][section.second]["right"].push_back((game / "cam2" / "right.mp4").string());
  }
  if (!write(game / "cam1" / "left.mp4", "left") || !write(game / "cam2" / "right.mp4", "right") ||
      !write(game / "config.yaml", YAML::Dump(original)))
    return false;
  const StitchingExperimentSettings settings{900, 2, "00:00:08", std::nullopt, "2k"};
  const auto baseline = CreateStitchingExperimentWorkspace(game, root / "inherited-camera", settings, 1);
  StitchingExperimentSettings varied = settings;
  varied.control_points = 1200;
  varied.control_point_resolution = "native";
  varied.rink_rotation_degrees = std::array<double, 3>{0, 1, 2};
  const auto candidate = CreateStitchingExperimentWorkspace(game, root / "inherited-camera", varied, 2);
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
  // The CLI removes this key when 00:00:08 equals the inherited user setting.
  // Its absence must not conflict with the candidate's explicit reference time.
  resolved["stitching"].remove("stitch_frame_time");
  resolved["game"]["stitching"]["frame_offsets"]["left"] = 0;
  resolved["game"]["stitching"]["frame_offsets"]["right"] = 1;
  resolved["hstream_ui"]["stitching_calibration"]["status"] = "complete";
  resolved["rink"]["stitched_output_generation"] = *output_generation;
  if (!write(directory / "config.yaml", YAML::Dump(resolved)))
    return false;
  const auto context = player_frame_source_context(resolved, 8 * kPlayerFrameSecond);
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
      {"decode_anchor_ns", "8000000000"}};
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
      frame.pair.cameras[camera_index] = {plan.sources[camera_index].path, (8 + index) * kPlayerFrameSecond};
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
  if (!prepared->available || !frozen_camera.ok() || *frozen_camera != camera ||
      frozen["stitching"]["stitch_frame_time"].IsDefined() ||
      frozen["stitching"]["control_point_resolution"].as<std::string>() != "native" ||
      !validate_player_frame_selection_sources(frozen).ok())
    return false;

  varied.control_points = 1500;
  varied.control_point_resolution = "1k";
  varied.stitch_frame_time = "00:00:08.000";
  varied.rink_rotation_degrees = std::array<double, 3>{0, -2, 3};
  const auto reused = CreateStitchingExperimentWorkspace(game, root / "inherited-camera", varied, 3);
  if (!reused.ok() ||
      !expect(
          ReuseStitchingExperimentPlayerSelection(*candidate, *reused).ok(),
          "a frozen selection must be reusable before its owner's solve completes"))
    return false;
  const YAML::Node reused_config = YAML::LoadFile((reused->game_directory / "config.yaml").string());
  if (!expect(
          reused_config["stitching"]["calibration_frame_selection"]["fingerprint"].as<std::string>() ==
                  plan.fingerprint &&
              !reused_config["stitching"]["stitch_frame_time"].IsDefined() &&
              reused_config["hstream_ui"]["stitching_calibration"]["control_points"].as<int>() == 1500 &&
              reused_config["stitching"]["control_point_resolution"].as<std::string>() == "1k" &&
              reused_config["stitching"]["projection_framing"]["rotation_degrees"][1].as<double>() == -2,
          "reuse must preserve exact frames and reference spelling while changing only solve settings"))
    return false;
  YAML::Node conflicting = YAML::Clone(reused_config);
  conflicting["game"]["stitching"]["frame_offsets"]["left"] = 3;
  if (!write(reused->game_directory / "config.yaml", YAML::Dump(conflicting)) ||
      !expect(
          !ReuseStitchingExperimentPlayerSelection(*candidate, *reused).ok(),
          "reuse must reject changed synchronization instead of replacing it"))
    return false;
  conflicting = YAML::Clone(reused_config);
  conflicting["game"]["videos"]["left"][0] = "cam2/right.mp4";
  if (!write(reused->game_directory / "config.yaml", YAML::Dump(conflicting)) ||
      !expect(
          !ReuseStitchingExperimentPlayerSelection(*candidate, *reused).ok(),
          "reuse must reject changed source roles instead of replacing them"))
    return false;
  if (!write(reused->game_directory / "config.yaml", YAML::Dump(reused_config)))
    return false;

  const auto promote = [&] {
    return BuildStitchingExperimentSelectionConfig(candidate->game_directory / "config.yaml", game / "config.yaml");
  };
  const auto validates_after_promotion = [&](const absl::StatusOr<std::string>& promoted) {
    if (!promoted.ok()) {
      std::cerr << promoted.status() << '\n';
      return false;
    }
    const YAML::Node config = YAML::Load(*promoted);
    for (const auto& section : {std::make_pair("game", "videos"), std::make_pair("hstream_ui", "video_roles")}) {
      if (config[section.first][section.second]["left"][0].as<std::string>() != "cam1/left.mp4" ||
          config[section.first][section.second]["right"][0].as<std::string>() != "cam2/right.mp4")
        return false;
    }
    return config["stitching"]["calibration_frame_selection"]["fingerprint"].as<std::string>() == plan.fingerprint &&
        validate_player_frame_selection_sources(config).ok();
  };
  if (!expect(validates_after_promotion(promote()), "absolute roles must replay the same frozen plan after promotion"))
    return false;
  const auto promoted = promote();
  if (!promoted.ok() || !write(game / "config.yaml", *promoted))
    return false;
  YAML::Node old_baseline = YAML::LoadFile((candidate->game_directory / "config.yaml").string());
  old_baseline["stitching"].remove("calibration_frame_selection");
  old_baseline["stitching"].remove("calibration_frame_inputs_fingerprint");
  if (!write(candidate->game_directory / "old-baseline.yaml", YAML::Dump(old_baseline)))
    return false;
  const auto replaced_selection =
      BuildStitchingExperimentSelectionConfig(candidate->game_directory / "old-baseline.yaml", game / "config.yaml");
  if (!expect(
          !replaced_selection.ok() && absl::IsFailedPrecondition(replaced_selection.status()),
          "a same-count ordinary historical baseline must not silently replace Main's selected player frames"))
    return false;
  old_baseline["stitching"]["calibration_frame_count"] = plan.selected.size() + 1;
  old_baseline["hstream_ui"]["stitching_calibration"]["frame_count"] = plan.selected.size() + 1;
  if (!write(candidate->game_directory / "old-baseline.yaml", YAML::Dump(old_baseline)) ||
      !expect(
          BuildStitchingExperimentSelectionConfig(candidate->game_directory / "old-baseline.yaml", game / "config.yaml")
              .ok(),
          "selecting an explicitly different frame count may replace the old selection"))
    return false;
  YAML::Node pending_view = YAML::Load(*promoted);
  pending_view["hstream_ui"]["stitching_calibration"]["reframe"]["source_generation"] = "old-source";
  if (!write(game / "config.yaml", YAML::Dump(pending_view)))
    return false;
  const auto over_pending_view = promote();
  if (!expect(
          over_pending_view.ok() &&
              !YAML::Load(*over_pending_view)["hstream_ui"]["stitching_calibration"]["reframe"].IsDefined(),
          "promoting a complete candidate must replace Main's pending view request"))
    return false;
  const auto fresh_candidate = CreateStitchingExperimentWorkspace(game, root / "pending-view-candidate", varied, 1);
  if (!expect(
          fresh_candidate.ok() &&
              !YAML::LoadFile((fresh_candidate->game_directory / "config.yaml")
                                  .string())["hstream_ui"]["stitching_calibration"]["reframe"]
                   .IsDefined(),
          "new candidates must not inherit a reframe request bound to Main"))
    return false;
  if (!write(game / "config.yaml", *promoted))
    return false;
  const auto retained = CreateStitchingExperimentWorkspace(game, root / "inherited-camera", varied, 4);
  if (!expect(retained.ok(), "a promoted plan must be retained by later experiment candidates"))
    return false;
  const auto retained_fingerprint = ReusableStitchingExperimentSelectionFingerprint(retained->game_directory, varied);
  if (!expect(
          retained_fingerprint.ok() && *retained_fingerprint == plan.fingerprint,
          "new candidates must retain the promoted fingerprint"))
    return false;
  StitchingExperimentSettings changed_reference = varied;
  changed_reference.stitch_frame_time = "00:00:09";
  if (!expect(
          !CreateStitchingExperimentWorkspace(game, root / "inherited-camera", changed_reference, 5).ok(),
          "same-count candidates must reject a reference conflict"))
    return false;
  StitchingExperimentSettings changed_count = changed_reference;
  changed_count.frame_count = 3;
  const auto replacement = CreateStitchingExperimentWorkspace(game, root / "inherited-camera", changed_count, 6);
  if (!expect(replacement.ok(), "an explicit different count may start a fresh selection"))
    return false;
  const auto replacement_fingerprint =
      ReusableStitchingExperimentSelectionFingerprint(replacement->game_directory, changed_count);
  if (!expect(
          replacement_fingerprint.ok() && replacement_fingerprint->empty(),
          "only an explicit different count clears the inherited plan"))
    return false;
  if (!write(game / "config.yaml", YAML::Dump(original)))
    return false;

  std::error_code error;
  fs::create_symlink(game / "cam1" / "left.mp4", game / "left-alias.mp4", error);
  if (error)
    return false;
  YAML::Node alias = YAML::Clone(original);
  alias["game"]["videos"]["left"][0] = "left-alias.mp4";
  alias["hstream_ui"]["video_roles"]["left"][0] = (game / "left-alias.mp4").string();
  if (!write(game / "config.yaml", YAML::Dump(alias)) ||
      !expect(validates_after_promotion(promote()), "equivalent camera aliases must preserve the frozen plan"))
    return false;

  YAML::Node changed = YAML::Clone(original);
  changed["hstream_ui"]["video_roles"]["left"][0] = (game / "cam2" / "right.mp4").string();
  if (!write(game / "config.yaml", YAML::Dump(changed)))
    return false;
  const auto rejected = promote();
  if (!expect(
          !rejected.ok() && absl::IsAborted(rejected.status()), "promotion must not rewrite a different camera input"))
    return false;

  YAML::Node ordinary = YAML::Clone(frozen);
  ordinary["stitching"].remove("calibration_frame_selection");
  if (!write(candidate->game_directory / "ordinary.yaml", YAML::Dump(ordinary)))
    return false;
  const auto ordinary_promotion =
      BuildStitchingExperimentSelectionConfig(candidate->game_directory / "ordinary.yaml", game / "config.yaml");
  if (!expect(
          ordinary_promotion.ok() &&
              YAML::Dump(YAML::Load(*ordinary_promotion)["hstream_ui"]["video_roles"]) ==
                  YAML::Dump(changed["hstream_ui"]["video_roles"]),
          "ordinary promotion must preserve the destination's camera roles"))
    return false;

  if (!write(game / "config.yaml", YAML::Dump(original)) || !fs::remove(game / "cam1" / "left.mp4") ||
      !fs::remove(game / "cam2" / "right.mp4"))
    return false;
  if (!expect(promote().ok(), "identical destination paths must not require available media for artifact promotion"))
    return false;
  return expect(
      BuildStitchingExperimentSelectionConfig(candidate->game_directory / "ordinary.yaml", game / "config.yaml").ok(),
      "ordinary artifact promotion must not require available media");
}

bool experiment_resolutions(const fs::path& root) {
  const fs::path game = root / "resolution-game";
  YAML::Node source;
  source["stitching"]["control_point_resolution"] = "1k";
  source["hstream_ui"]["generated_control_point_resolution"]["generated"] = "1k";
  source["hstream_ui"]["generated_control_point_resolution"]["previous"] = "2k";
  auto calibration = source["hstream_ui"]["stitching_calibration"];
  calibration["status"] = "complete";
  calibration["invalidation_id"] = "saved-main";
  calibration["backend_generation"]["invalidation_id"] = "saved-main";
  calibration["backend_generation"]["control_point_resolution"] = "native";
  if (!write(game / "cam1" / "left.mp4", "left") || !write(game / "cam2" / "right.mp4", "right") ||
      !write(game / "config.yaml", YAML::Dump(source)))
    return false;
  StitchingExperimentSettings settings{100, 2, "00:00:00", std::nullopt, "2k"};
  auto main = MainStitchingExperimentWorkspace(game, settings);
  bool ok = expect(
      main.ok() && main->has_value() && (**main).settings.control_point_resolution == "native",
      "Main reports its saved generation's Native size instead of the displayed experiment size");
  int sequence = 0;
  for (const std::string size : {"native", "1k", "2k"}) {
    settings.control_point_resolution = size;
    const auto workspace = CreateStitchingExperimentWorkspace(game, root / "resolution-session", settings, ++sequence);
    if (!expect(workspace.ok(), "explicit resolution candidate must be created"))
      return false;
    const YAML::Node config = YAML::LoadFile((workspace->game_directory / "config.yaml").string());
    ok &= expect(
        workspace->settings.control_point_resolution == size &&
            config["stitching"]["control_point_resolution"].as<std::string>() == size &&
            !config["hstream_ui"]["generated_control_point_resolution"].IsDefined(),
        "candidate size must override copied generated values and remain in its settings");
    const auto promoted =
        BuildStitchingExperimentSelectionConfig(workspace->game_directory / "config.yaml", game / "config.yaml");
    if (!expect(promoted.ok(), "candidate size must be promotable"))
      return false;
    YAML::Node selected = YAML::Load(*promoted);
    ok &= expect(
        !hm::stitching::restore_generated_control_point_resolution(selected) &&
            selected["stitching"]["control_point_resolution"].as<std::string>() == size,
        "Main's previous generated marker must not undo the promoted size");
  }
  settings.control_point_resolution.reset();
  const auto inherited = CreateStitchingExperimentWorkspace(game, root / "resolution-session", settings, ++sequence);
  ok &= expect(
      inherited.ok() && inherited->settings.control_point_resolution == "1k",
      "an inherited size already materialized in the source config is frozen when queued");
  calibration.remove("backend_generation");
  if (!write(game / "config.yaml", YAML::Dump(source)))
    return false;
  main = MainStitchingExperimentWorkspace(game, settings);
  ok &= expect(
      main.ok() && main->has_value() && (**main).settings.control_point_resolution == "1k",
      "Main without a generation claim reports its saved canonical size");
  source["stitching"].remove("control_point_resolution");
  source["hstream_ui"].remove("generated_control_point_resolution");
  if (!write(game / "config.yaml", YAML::Dump(source)))
    return false;
  const auto legacy = CreateStitchingExperimentWorkspace(game, root / "resolution-session", settings, ++sequence);
  ok &= expect(
      legacy.ok() && !legacy->settings.control_point_resolution,
      "missing source size keeps inheritance instead of guessing a user-level default");
  settings.control_point_resolution = "native";
  main = MainStitchingExperimentWorkspace(game, settings);
  ok &= expect(
      main.ok() && main->has_value() && !(**main).settings.control_point_resolution,
      "legacy Main without saved size must not copy the current experiment selector");
  settings.control_point_resolution = "invalid";
  ok &= expect(
      !CreateStitchingExperimentWorkspace(game, root / "resolution-session", settings, ++sequence).ok(),
      "invalid experiment image sizes are rejected");
  return ok;
}

bool edited_match_workspace(const fs::path& root) {
  using namespace hm::stitching;
  const fs::path original_game = root / "edited-original";
  if (!write(original_game / "left.mp4", "left") || !write(original_game / "right.mp4", "right") ||
      !write(original_game / "left_calibration.json", "{}"))
    return false;
  YAML::Node config = YAML::Load(
      "game: {videos: {left: [left.mp4], right: [right.mp4]}}\n"
      "stitching: {control_point_matcher: superpoint-lightglue, calibration_frame_count: 1}\n"
      "hstream_ui: {stitching_calibration: {invalidation_id: source-owner, status: complete, control_points: 100, frame_count: 1}}\n");
  if (!write(original_game / "config.yaml", YAML::Dump(config)))
    return false;
  const auto candidate =
      CreateStitchingExperimentWorkspace(original_game, root / "source-session", {100, 1, "00:00:00", std::nullopt}, 1);
  if (!expect(candidate.ok(), "editable fixture starts with a real linked-media workspace"))
    return false;
  const fs::path game = candidate->game_directory;
  config = YAML::LoadFile((game / "config.yaml").string());
  fs::create_symlink(original_game / "right.mp4", game / "right-99.mp4");
  CalibrationMatchSet automatic;
  const auto context = CalibrationMatchSourceContext(config, game);
  if (!context.ok()) {
    std::cerr << "Cannot identify edited fixture sources: " << context.status() << '\n';
    return false;
  }
  automatic.source_context = *context;
  CalibrationMatchFrame frame;
  for (size_t camera = 0; camera < 2; ++camera) {
    frame.images[camera] = game / (camera ? "right.png" : "left.png");
    frame.sizes[camera] = {80, 60};
    frame.source_paths[camera] = (game / (camera ? "right.mp4" : "left.mp4")).string();
    if (!cv::imwrite(frame.images[camera].string(), cv::Mat(60, 80, CV_8UC3, cv::Scalar(20, 40, 60))))
      return false;
  }
  for (int i = 0; i < 12; ++i)
    frame.matches.push_back({{float(i + 10), 20}, {float(i + 12), 22}, 0.9f});
  automatic.frames.push_back(frame);
  const auto published = PublishCalibrationMatches(game, automatic);
  if (!expect(published.ok(), "automatic editable fixture must publish"))
    return false;
  config["hstream_ui"]["stitching_calibration"]["match_snapshot"] = *published;
  const std::string original_config = YAML::Dump(config);
  if (!write(game / "config.yaml", original_config))
    return false;
  auto source_matches = LoadCalibrationMatches(game, *published);
  if (!source_matches.ok()) {
    std::cerr << "Cannot load edited fixture matches: " << source_matches.status() << '\n';
    return false;
  }
  auto edited = *source_matches;
  edited.frames[0].matches[0].left.x += 2;
  edited.frames[0].matches.erase(edited.frames[0].matches.begin() + 1);
  StitchingExperimentWorkspace source = *candidate;
  const auto workspace =
      CreateEditedStitchingExperimentWorkspace(source, root / "edited-session", *published, edited, 1);
  if (!expect(workspace.ok(), "edited matches must create an isolated replacement candidate")) {
    if (!workspace.ok())
      std::cerr << workspace.status() << '\n';
    return false;
  }
  const auto saved = LoadCalibrationMatches(workspace->game_directory, *workspace->settings.manual_control_points);
  bool ok = expect(
      saved.ok() && saved->manual && saved->automatic_fingerprint == *published &&
          saved->frames[0].matches.size() == 11 && saved->frames[0].matches[0].left.x == 12,
      "replacement preserves edits and original automatic provenance");
  ok &= expect(
      fs::read_symlink(workspace->game_directory / "left.mp4") == fs::canonical(original_game / "left.mp4") &&
          fs::read_symlink(workspace->game_directory / "left_calibration.json") ==
              fs::canonical(original_game / "left_calibration.json"),
      "cloned media and calibration sidecars point directly to the original regular files");
  ok &= expect(
      !fs::exists(workspace->game_directory / "right-99.mp4"),
      "workspace clones only carry explicitly configured camera videos");
  if (saved.ok()) {
    auto again = *saved;
    again.frames[0].matches[0].right.y += 1;
    const auto subsequent =
        CreateEditedStitchingExperimentWorkspace(*workspace, root / "edited-session", saved->fingerprint, again, 5);
    ok &= expect(subsequent.ok(), "a manual replacement can be edited again");
    if (subsequent.ok()) {
      const auto matches =
          LoadCalibrationMatches(subsequent->game_directory, *subsequent->settings.manual_control_points);
      ok &= expect(
          matches.ok() && matches->automatic_fingerprint == *published,
          "successive edits retain the original automatic match set");
    }
  }
  const auto selected =
      BuildStitchingExperimentSelectionConfig(workspace->game_directory / "config.yaml", game / "config.yaml");
  ok &= expect(
      selected.ok() &&
          YAML::Load(*selected)["stitching"]["manual_control_points"].as<std::string>() ==
              *workspace->settings.manual_control_points,
      "promotion configuration retains the selected manual replacement identity");
  std::ifstream original(game / "config.yaml");
  const std::string after((std::istreambuf_iterator<char>(original)), std::istreambuf_iterator<char>());
  ok &= expect(after == original_config, "editing preserves the original candidate configuration");
  ok &= expect(
      !CreateEditedStitchingExperimentWorkspace(source, root / "edited-session", std::string(64, 'a'), edited, 2).ok(),
      "stale editor snapshot must fail instead of replacing another solve");
  edited.frames[0].source_seconds[0] += 1;
  ok &= expect(
      !CreateEditedStitchingExperimentWorkspace(source, root / "edited-session", *published, edited, 3).ok(),
      "edited input identities cannot change the inspected frame pair");
  ok &= expect(
      !CreateStitchingExperimentWorkspace(workspace->game_directory, root / "edited-session", source.settings, 6).ok(),
      "general workspace creation must still reject linked inputs outside the selected game");
  const auto plain = CreateStitchingExperimentEditableCopy(*workspace, root / "edited-session", 4);
  if (!expect(plain.ok(), "a fresh automatic candidate can be derived from an edited candidate"))
    return false;
  const YAML::Node plain_config = YAML::LoadFile((plain->game_directory / "config.yaml").string());
  ok &= expect(
      !plain_config["stitching"]["manual_control_points"] &&
          !plain_config["hstream_ui"]["stitching_calibration"]["match_snapshot"],
      "fresh automatic candidates must clear inherited manual and inspection references");
  auto stale = *workspace;
  stale.invalidation_id = "stale-owner";
  ok &= expect(
      !CreateStitchingExperimentEditableCopy(stale, root / "edited-session", 7).ok(),
      "automatic copies must reject a stale source generation");
  YAML::Node invalid = YAML::LoadFile((workspace->game_directory / "config.yaml").string());
  invalid["game"]["videos"]["left"][0] = (original_game / "left.mp4").string();
  if (!write(workspace->game_directory / "config.yaml", YAML::Dump(invalid)))
    return false;
  ok &= expect(
      !CreateStitchingExperimentEditableCopy(*workspace, root / "edited-session", 8).ok(),
      "clones must reject configured media names outside their workspace");
  invalid["game"]["videos"]["left"][0] = "nested/../left.mp4";
  if (!write(workspace->game_directory / "config.yaml", YAML::Dump(invalid)))
    return false;
  ok &= expect(
      !CreateStitchingExperimentEditableCopy(*workspace, root / "edited-session", 9).ok(),
      "clones must reject parent traversal even when it resolves to an owned media link");
  fs::create_directory_symlink(original_game, workspace->game_directory / "linked-directory");
  invalid["game"]["videos"]["left"][0] = "linked-directory/left.mp4";
  if (!write(workspace->game_directory / "config.yaml", YAML::Dump(invalid)))
    return false;
  ok &= expect(
      !CreateStitchingExperimentEditableCopy(*workspace, root / "edited-session", 10).ok(),
      "clones must reject configured videos reached through a directory symlink");
  return ok;
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

  if (!edited_match_workspace(root))
    return 24;

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
          "hstream_ui:\n"
          "  projection_crop_geometry: inherited-old-alignment\n"
          "stitching:\n"
          "  control_points: [old-cache]\n"
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
  ok &= expect(experiment_resolutions(root), "experiment image sizes must persist and promote independently");
  ok &= expect(durable_workspace_publication(root), "queued workspaces must be durable before catalog publication");
  ok &= expect(ordinary_frame_inspection(*workspace), "ordinary calibration inspection must remain bound to its row");
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
      "ordinary workspaces without a saved selection must remain ordinary");
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
  std::string selected_pto = "p f19 v180\ni w3840 h2160 f0 v108 y-25\ni w3840 h2160 f0 v108 y25\n";
  for (size_t i = 0; i < 16 * 2000; ++i)
    selected_pto += "c n0 N1 x123.456789 y234.567891 X345.678912 Y456.789123 t0\n";
  ok &= expect(selected_pto.size() > 1024 * 1024, "multi-frame promotion fixture must exceed the old PTO read cap");
  ok &= expect(
      write(workspace->game_directory / "autooptimiser_out.pto", selected_pto),
      "selected candidate geometry fixture must publish");
  const auto selected_config =
      BuildStitchingExperimentSelectionConfig(workspace->game_directory / "config.yaml", game / "config.yaml");
  ok &= expect(selected_config.ok(), "selected candidate config must be mergeable");
  if (selected_config.ok()) {
    const YAML::Node selected = YAML::Load(*selected_config);
    const YAML::Node selected_rink = selected["rink"];
    const auto selected_framing = hm::stitching::read_stitch_projection_framing(selected);
    ok &= expect(
        selected_framing.ok() &&
            hm::stitching::projection_crop_reviewed(
                selected, hm::stitching::projection_crop_geometry(selected_pto, *selected_framing)),
        "explicit candidate selection must accept its actual crop geometry so first Play reuses the chosen maps");
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
                workspace->game_directory / "player-frame-inspection" / plan.fingerprint / "left_1.jpg" &&
            inspected->frames[1].match_images[0] ==
                workspace->game_directory / "calibration-frame-inspection" / workspace->invalidation_id /
                    "points_1.jpg",
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
