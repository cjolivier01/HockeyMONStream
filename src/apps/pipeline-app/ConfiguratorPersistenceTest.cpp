#include "src/apps/pipeline-app/StitcherOnePassConfig.h"
#include "src/apps/pipeline-app/configurator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>

#include <gst/gst.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/UserConfig.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/GameConfig.h"

GST_DEBUG_CATEGORY(NVDS_APP);

namespace {

namespace fs = std::filesystem;

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

} // namespace

int main() {
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);
  bool ok = true;
  const auto rotation_value = [](const YAML::Node& config) {
    return hm::configurator_internal::effective_stitch_output_rotation(config);
  };
  const auto missing_rotation = rotation_value(YAML::Load("{}"));
  const auto global_null_rotation = rotation_value(YAML::Load("stitching:\n  post_stitch_rotate_degrees: null\n"));
  const auto global_rotation = rotation_value(YAML::Load("stitching:\n  post_stitch_rotate_degrees: 2.5\n"));
  const auto pipeline_null_fallback = rotation_value(
      YAML::Load(
          "stitching:\n  post_stitch_rotate_degrees: 2.5\n"
          "pipeline:\n  hmstitcher:\n    post-stitch-rotate-degrees: null\n"));
  const auto pipeline_override = rotation_value(
      YAML::Load(
          "stitching:\n  post_stitch_rotate_degrees: invalid-lower-priority-value\n"
          "pipeline:\n  hmstitcher:\n    post-stitch-rotate-degrees: 5\n"));
  const auto underscored_pipeline_override = rotation_value(
      YAML::Load(
          "stitching:\n  post_stitch_rotate_degrees: invalid-lower-priority-value\n"
          "pipeline:\n  hmstitcher:\n    post_stitch_rotate_degrees: 7.5\n"));
  const auto underscored_pipeline_null_fallback = rotation_value(
      YAML::Load(
          "stitching:\n  post_stitch_rotate_degrees: 2.5\n"
          "pipeline:\n  hmstitcher:\n    post_stitch_rotate_degrees: null\n"));
  const auto all_null_rotation = rotation_value(
      YAML::Load(
          "stitching:\n  post_stitch_rotate_degrees: null\n"
          "pipeline:\n  hmstitcher:\n    post-stitch-rotate-degrees: null\n"
          "    post_stitch_rotate_degrees: null\n"));
  const auto malformed_pipeline = rotation_value(
      YAML::Load(
          "stitching:\n  post_stitch_rotate_degrees: 2.5\n"
          "pipeline:\n  hmstitcher:\n    post-stitch-rotate-degrees: invalid\n"));
  const auto non_finite_pipeline =
      rotation_value(YAML::Load("pipeline:\n  hmstitcher:\n    post-stitch-rotate-degrees: .nan\n"));
  const auto malformed_underscored_pipeline =
      rotation_value(YAML::Load("pipeline:\n  hmstitcher:\n    post_stitch_rotate_degrees: invalid\n"));
  const auto non_finite_underscored_pipeline =
      rotation_value(YAML::Load("pipeline:\n  hmstitcher:\n    post_stitch_rotate_degrees: .inf\n"));
  const auto malformed_global = rotation_value(YAML::Load("stitching:\n  post_stitch_rotate_degrees: invalid\n"));
  const auto non_finite_global = rotation_value(YAML::Load("stitching:\n  post_stitch_rotate_degrees: -.inf\n"));
  ok &= expect(
      missing_rotation.ok() && *missing_rotation == 0.0 && global_null_rotation.ok() && *global_null_rotation == 0.0 &&
          global_rotation.ok() && *global_rotation == 2.5 && pipeline_null_fallback.ok() &&
          *pipeline_null_fallback == 2.5 && pipeline_override.ok() && *pipeline_override == 5.0 &&
          underscored_pipeline_override.ok() && *underscored_pipeline_override == 7.5 &&
          underscored_pipeline_null_fallback.ok() && *underscored_pipeline_null_fallback == 2.5 &&
          all_null_rotation.ok() && *all_null_rotation == 0.0 && absl::IsInvalidArgument(malformed_pipeline.status()) &&
          absl::IsInvalidArgument(non_finite_pipeline.status()) &&
          absl::IsInvalidArgument(malformed_underscored_pipeline.status()) &&
          absl::IsInvalidArgument(non_finite_underscored_pipeline.status()) &&
          absl::IsInvalidArgument(malformed_global.status()) && absl::IsInvalidArgument(non_finite_global.status()),
      "Stitch output rotation must use the highest-priority non-null value and reject an invalid active value");
  ok &= expect(
      !hm::OnePassCalibrationRequired(
          /*one_pass_mode=*/false, /*stitching_configured=*/false, /*field_mask_configured=*/false) &&
          !hm::OnePassCalibrationRequired(
              /*one_pass_mode=*/true, /*stitching_configured=*/true, /*field_mask_configured=*/true) &&
          hm::OnePassCalibrationRequired(
              /*one_pass_mode=*/true, /*stitching_configured=*/false, /*field_mask_configured=*/false) &&
          hm::OnePassCalibrationRequired(
              /*one_pass_mode=*/true, /*stitching_configured=*/true, /*field_mask_configured=*/false),
      "One-pass calibration must include missing mappings and mask-only resume state");
  const fs::path root = fs::temp_directory_path() / ("configurator-persistence-test-" + std::to_string(::getpid()));
  fs::remove_all(root);
  const std::string original_home = ::getenv("HOME") ? ::getenv("HOME") : "";
  const std::string original_game_root = ::getenv("HM_GAME_DIR") ? ::getenv("HM_GAME_DIR") : "";
  const std::string original_output_root = ::getenv("HM_OUTPUT_WORK_DIR") ? ::getenv("HM_OUTPUT_WORK_DIR") : "";
  const std::string original_xdg_runtime_dir = ::getenv("XDG_RUNTIME_DIR") ? ::getenv("XDG_RUNTIME_DIR") : "";
  const fs::path test_home = root / "home";
  fs::create_directories(test_home);
  ::setenv("HOME", test_home.c_str(), 1);
  ::unsetenv("HM_OUTPUT_WORK_DIR");

  const auto bundled_baseline = hm::baseline_config::load();
  YAML::Node playtracker_base = YAML::Load(R"(
play-tracker:
  preserve-me: yes
  live-boxes:
    - name: current_roi
      preserve-fast: 1
    - name: current_roi_aspect
      preserve-follower: 1
)");
  auto effective_playtracker = bundled_baseline.ok()
      ? hm::configurator_internal::build_effective_playtracker_config(
            bundled_baseline->values, {}, /*native_base_rank=*/0, playtracker_base)
      : absl::StatusOr<YAML::Node>(bundled_baseline.status());
  if (effective_playtracker.ok()) {
    const YAML::Node play_tracker = (*effective_playtracker)["play-tracker"];
    const YAML::Node fast = play_tracker["live-boxes"][0];
    const YAML::Node follower = play_tracker["live-boxes"][1];
    ok &= expect(
        play_tracker["preserve-me"].as<std::string>() == "yes" && fast["preserve-fast"].as<int>() == 1 &&
            follower["preserve-follower"].as<int>() == 1 && play_tracker["camera-name"].as<std::string>() == "GoPro" &&
            play_tracker["no-wide-start"].as<bool>() && play_tracker["ignore-largest-bbox"].as<bool>() &&
            play_tracker["min-considered-group-velocity"].as<double>() == 3.0 &&
            play_tracker["group-ratio-threshold"].as<double>() == 0.5 &&
            play_tracker["group-velocity-speed-ratio"].as<double>() == 0.3 &&
            play_tracker["scale-speed-constraints"].as<double>() == 3.0 &&
            play_tracker["nonstop-delay-count"].as<int>() == 2 &&
            play_tracker["overshoot-scale-speed-ratio"].as<double>() == 0.7 &&
            play_tracker["overshoot-stop-delay-count"].as<int>() == 6 &&
            play_tracker["max-speed-ratio-x"].as<double>() == 1.0 &&
            play_tracker["max-speed-ratio-y"].as<double>() == 1.0 &&
            play_tracker["max-accel-ratio-x"].as<double>() == 1.0 &&
            play_tracker["max-accel-ratio-y"].as<double>() == 1.0 &&
            play_tracker["follower-box-min-height-ratio"].as<double>() == 0.2 &&
            play_tracker["zoom-in-aggressiveness"].as<int>() == 25 &&
            fast["time-to-dest-speed-limit-frames"].as<int>() == 20 &&
            fast["time-to-dest-stop-speed-threshold"].as<double>() == 0.25 &&
            fast["resizing-stop-on-dir-change-delay"].as<int>() == 4 &&
            fast["resizing-cancel-stop-on-opposite-dir"].as<bool>() &&
            fast["resizing-stop-cancel-hysteresis-frames"].as<int>() == 10 &&
            fast["resizing-stop-delay-cooldown-frames"].as<int>() == 2 &&
            fast["resizing-time-to-dest-speed-limit-frames"].as<int>() == 10 &&
            fast["resizing-time-to-dest-stop-speed-threshold"].as<double>() == 0.25 &&
            follower["stop-translation-on-dir-change-delay"].as<int>() == 10 &&
            follower["cancel-stop-on-opposite-dir"].as<bool>() &&
            follower["cancel-stop-hysteresis-frames"].as<int>() == 2 &&
            follower["stop-delay-cooldown-frames"].as<int>() == 2 &&
            follower["post-nonstop-stop-delay-count"].as<int>() == 6 &&
            follower["sticky-size-ratio-to-frame-width"].as<double>() == 10.0 &&
            follower["sticky-translation-gaussian-mult"].as<double>() == 5.0 &&
            follower["unsticky-translation-size-ratio"].as<double>() == 0.75 &&
            follower["scale-dest-width"].as<double>() == 1.45 && follower["scale-dest-height"].as<double>() == 1.45,
        "Every native playtracker default represented by baseline.yaml must be materialized after preserving structure");
  } else {
    ok &= expect(false, effective_playtracker.status().ToString().c_str());
  }
  if (bundled_baseline.ok()) {
    YAML::Node overlaid = YAML::Clone(bundled_baseline->values);
    overlaid["rink"]["camera"]["stop_on_dir_change_delay"] = 17;
    overlaid["rink"]["camera"]["zoom_in_aggressiveness"] = 75;
    overlaid["rink"]["camera"]["breakaway_detection"]["overshoot_stop_delay_count"] = 12;
    const hm::configurator_internal::ConfigLeafRanks explicit_overrides = {
        {"rink.camera.stop_on_dir_change_delay", 1},
        {"rink.camera.zoom_in_aggressiveness", 1},
        {"rink.camera.breakaway_detection.overshoot_stop_delay_count", 1},
    };
    const auto overlaid_playtracker = hm::configurator_internal::build_effective_playtracker_config(
        overlaid, explicit_overrides, /*native_base_rank=*/0, playtracker_base);
    ok &= expect(
        overlaid_playtracker.ok() &&
            (*overlaid_playtracker)["play-tracker"]["overshoot-stop-delay-count"].as<int>() == 12 &&
            (*overlaid_playtracker)["play-tracker"]["zoom-in-aggressiveness"].as<int>() == 75 &&
            (*overlaid_playtracker)["play-tracker"]["live-boxes"][1]["stop-translation-on-dir-change-delay"]
                    .as<int>() == 17,
        "Merged user/game/CLI values must replace bundled defaults in the materialized native tracker config");

    YAML::Node custom_base = YAML::Clone(playtracker_base);
    custom_base["play-tracker"]["no-wide-start"] = false;
    custom_base["play-tracker"]["live-boxes"][1]["sticky-translation-gaussian-mult"] = 9.5;
    const auto custom_only = hm::configurator_internal::build_effective_playtracker_config(
        bundled_baseline->values, {}, /*native_base_rank=*/0, custom_base);
    YAML::Node different_effective = YAML::Clone(bundled_baseline->values);
    different_effective["rink"]["camera"]["sticky_translation_gaussian_mult"] = 7.25;
    const hm::configurator_internal::ConfigLeafRanks different_explicit = {
        {"rink.camera.sticky_translation_gaussian_mult", 1}};
    const auto explicit_different = hm::configurator_internal::build_effective_playtracker_config(
        different_effective, different_explicit, /*native_base_rank=*/0, custom_base);
    const hm::configurator_internal::ConfigLeafRanks equal_explicit = {
        {"rink.camera.sticky_translation_gaussian_mult", 1}};
    const auto explicit_equal = hm::configurator_internal::build_effective_playtracker_config(
        bundled_baseline->values, equal_explicit, /*native_base_rank=*/0, custom_base);
    ok &= expect(
        custom_only.ok() && !(*custom_only)["play-tracker"]["no-wide-start"].as<bool>() &&
            (*custom_only)["play-tracker"]["live-boxes"][1]["sticky-translation-gaussian-mult"].as<double>() == 9.5 &&
            explicit_different.ok() &&
            (*explicit_different)["play-tracker"]["live-boxes"][1]["sticky-translation-gaussian-mult"].as<double>() ==
                7.25 &&
            explicit_equal.ok() &&
            (*explicit_equal)["play-tracker"]["live-boxes"][1]["sticky-translation-gaussian-mult"].as<double>() == 5.0,
        "Native custom values must survive baseline fill unless an exact canonical overlay path is explicit");

    YAML::Node reordered_base = YAML::Clone(playtracker_base);
    YAML::Node reordered_boxes(YAML::NodeType::Sequence);
    reordered_boxes.push_back(reordered_base["play-tracker"]["live-boxes"][1]);
    YAML::Node additional_box(YAML::NodeType::Map);
    additional_box["name"] = "operator_extra";
    additional_box["preserve-extra"] = true;
    reordered_boxes.push_back(additional_box);
    reordered_boxes.push_back(reordered_base["play-tracker"]["live-boxes"][0]);
    reordered_base["play-tracker"]["live-boxes"] = reordered_boxes;
    const auto reordered = hm::configurator_internal::build_effective_playtracker_config(
        bundled_baseline->values, {}, /*native_base_rank=*/0, reordered_base);
    YAML::Node one_box = YAML::Clone(playtracker_base);
    YAML::Node one_box_sequence(YAML::NodeType::Sequence);
    YAML::Node operator_box(YAML::NodeType::Map);
    operator_box["name"] = "operator_only";
    one_box_sequence.push_back(operator_box);
    one_box["play-tracker"]["live-boxes"] = one_box_sequence;
    const auto one_box_effective = hm::configurator_internal::build_effective_playtracker_config(
        bundled_baseline->values, {}, /*native_base_rank=*/0, one_box);
    ok &= expect(
        reordered.ok() && (*reordered)["play-tracker"]["live-boxes"].size() == 3 &&
            (*reordered)["play-tracker"]["live-boxes"][0]["name"].as<std::string>() == "current_roi" &&
            (*reordered)["play-tracker"]["live-boxes"][0]["preserve-fast"].as<int>() == 1 &&
            (*reordered)["play-tracker"]["live-boxes"][1]["preserve-extra"].as<bool>() &&
            (*reordered)["play-tracker"]["live-boxes"][1]["time-to-dest-speed-limit-frames"].as<int>() == 20 &&
            (*reordered)["play-tracker"]["live-boxes"][2]["name"].as<std::string>() == "current_roi_aspect" &&
            (*reordered)["play-tracker"]["live-boxes"][2]["sticky-translation-gaussian-mult"].as<double>() == 5.0 &&
            one_box_effective.ok() && (*one_box_effective)["play-tracker"]["live-boxes"].size() == 1 &&
            (*one_box_effective)["play-tracker"]["live-boxes"][0]["sticky-translation-gaussian-mult"].as<double>() ==
                5.0,
        "Baseline fill must normalize native roles, preserve additional boxes, and retain one-box compatibility");

    YAML::Node malformed_effective = YAML::Clone(bundled_baseline->values);
    malformed_effective["play_tracker"]["no_wide_start"] = "not-a-boolean";
    const hm::configurator_internal::ConfigLeafRanks malformed_explicit = {{"play_tracker.no_wide_start", 1}};
    const auto malformed_canonical = hm::configurator_internal::build_effective_playtracker_config(
        malformed_effective, malformed_explicit, /*native_base_rank=*/0, playtracker_base);
    YAML::Node malformed_native = YAML::Clone(playtracker_base);
    malformed_native["play-tracker"]["no-wide-start"] = "not-a-boolean";
    const auto malformed_custom = hm::configurator_internal::build_effective_playtracker_config(
        bundled_baseline->values, {}, /*native_base_rank=*/0, malformed_native);
    YAML::Node invalid_zoom = YAML::Clone(bundled_baseline->values);
    invalid_zoom["rink"]["camera"]["zoom_in_aggressiveness"] = 101;
    const hm::configurator_internal::ConfigLeafRanks invalid_zoom_explicit = {
        {"rink.camera.zoom_in_aggressiveness", 1}};
    const auto invalid_zoom_config = hm::configurator_internal::build_effective_playtracker_config(
        invalid_zoom, invalid_zoom_explicit, /*native_base_rank=*/0, playtracker_base);
    YAML::Node negative_zoom = YAML::Clone(bundled_baseline->values);
    negative_zoom["rink"]["camera"]["zoom_in_aggressiveness"] = -1;
    const auto negative_zoom_config = hm::configurator_internal::build_effective_playtracker_config(
        negative_zoom, invalid_zoom_explicit, /*native_base_rank=*/0, playtracker_base);
    YAML::Node minimum_zoom = YAML::Clone(bundled_baseline->values);
    minimum_zoom["rink"]["camera"]["zoom_in_aggressiveness"] = 0;
    const auto minimum_zoom_config = hm::configurator_internal::build_effective_playtracker_config(
        minimum_zoom, invalid_zoom_explicit, /*native_base_rank=*/0, playtracker_base);
    YAML::Node maximum_zoom = YAML::Clone(bundled_baseline->values);
    maximum_zoom["rink"]["camera"]["zoom_in_aggressiveness"] = 100;
    const auto maximum_zoom_config = hm::configurator_internal::build_effective_playtracker_config(
        maximum_zoom, invalid_zoom_explicit, /*native_base_rank=*/0, playtracker_base);
    ok &= expect(
        !malformed_canonical.ok() && !malformed_custom.ok() && absl::IsInvalidArgument(invalid_zoom_config.status()) &&
            absl::IsInvalidArgument(negative_zoom_config.status()) && minimum_zoom_config.ok() &&
            maximum_zoom_config.ok() &&
            (*minimum_zoom_config)["play-tracker"]["zoom-in-aggressiveness"].as<int>() == 0 &&
            (*maximum_zoom_config)["play-tracker"]["zoom-in-aggressiveness"].as<int>() == 100,
        "Zoom aggressiveness must accept both boundaries and reject values below zero or above one hundred");
  }

  auto first_user_config = hm::user_config::load_or_create();
  const fs::path user_config_path = test_home / ".hstream" / "hstream.yaml";
  struct stat user_config_stat{};
  ok &= expect(
      first_user_config.ok() && fs::is_regular_file(user_config_path) &&
          (*first_user_config)[hm::user_config::kPathsKey][hm::user_config::kOutputRootKey].as<std::string>() ==
              (test_home / "hstream_output").string() &&
          !(*first_user_config)[hm::user_config::kPathsKey][hm::user_config::kGameRootKey] &&
          ::stat(user_config_path.c_str(), &user_config_stat) == 0 && (user_config_stat.st_mode & 0777) == 0600,
      "First config read must create a private user overlay containing only the HOME hstream_output default");

  const fs::path games = root / "games";
  const fs::path game_dir = games / "first-save";
  fs::create_directories(game_dir);
  ::setenv("HM_GAME_DIR", games.c_str(), 1);

  const fs::path baseline_root = root / "baseline";
  fs::create_directories(baseline_root);
  YAML::Node test_baseline =
      bundled_baseline.ok() ? YAML::Clone(bundled_baseline->values) : YAML::Node(YAML::NodeType::Map);
  test_baseline["pipeline"]["layered-value"] = "baseline";
  test_baseline["pipeline"]["baseline-only"] = "yes";
  test_baseline["stitching"]["stitch_frame_time"] = "00:00:07";
  std::ofstream(baseline_root / "baseline.yaml") << YAML::Dump(test_baseline) << '\n';
  YAML::Node user_overlay = first_user_config.ok() ? YAML::Clone(*first_user_config) : YAML::Node(YAML::NodeType::Map);
  user_overlay["pipeline"]["layered-value"] = "user";
  user_overlay["pipeline"]["user-only"] = "yes";
  user_overlay["stitching"]["stitch_frame_time"] = "00:00:08";
  user_overlay[hm::user_config::kPathsKey][hm::user_config::kGameRootKey] = games.string();
  user_overlay[hm::user_config::kPathsKey][hm::user_config::kOutputRootKey] = (root / "configured-output").string();
  std::ofstream(user_config_path) << YAML::Dump(user_overlay) << '\n';

  const fs::path mapping_structure_path = root / "mapping-structure.yaml";
  YAML::Node mapping_structure(YAML::NodeType::Map);
  mapping_structure["application"] = YAML::Node(YAML::NodeType::Map);
  mapping_structure["hmstitcher"] = YAML::Node(YAML::NodeType::Map);
  mapping_structure["hmplaycropper"] = YAML::Node(YAML::NodeType::Map);
  mapping_structure["ds-playtracker"] = YAML::Node(YAML::NodeType::Map);
  mapping_structure["ds-fieldmask"] = YAML::Node(YAML::NodeType::Map);
  mapping_structure["sink0"]["type"] = 3; // NV_DS_SINK_ENCODE_FILE
  std::ofstream(mapping_structure_path) << YAML::Dump(mapping_structure) << '\n';

  fs::create_directories(games / "mapping-defaults");
  hm::Configurator mapping_defaults("mapping-defaults", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_defaults_loaded = mapping_defaults.configure().ok() &&
      mapping_defaults.underlay_config("pipeline", mapping_structure_path.string());
  const absl::Status mapping_defaults_status = mapping_defaults_loaded
      ? mapping_defaults.apply_supported_baseline_mappings()
      : absl::InternalError("mapping defaults fixture did not load");
  const absl::Status mapping_defaults_second_status = mapping_defaults.apply_supported_baseline_mappings();
  const auto mapping_defaults_rotation =
      hm::configurator_internal::effective_stitch_output_rotation(mapping_defaults.config());
  const YAML::Node mapped_defaults = mapping_defaults.config()["pipeline"];
  ok &= expect(
      mapping_defaults_status.ok() && mapping_defaults_second_status.ok() && mapping_defaults_rotation.ok() &&
          *mapping_defaults_rotation == 0.0 && mapped_defaults["hmstitcher"]["enable"].as<int>() == 1 &&
          mapped_defaults["hmstitcher"]["minimize-blend"].as<int>() == 0 &&
          mapped_defaults["application"]["video-converter"].as<std::string>() == "nvvideoconvert" &&
          !mapped_defaults["hmstitcher"]["properties"]["max-output-width"].IsDefined() &&
          mapped_defaults["hmstitcher"]["stitch-compute-precision"].as<std::string>() == "fp32" &&
          mapped_defaults["hmplaycropper"]["no-crop"].as<int>() == 0 &&
          mapped_defaults["hmplaycropper"]["plot-play-tracking"].as<int>() == 0 &&
          mapped_defaults["hmplaycropper"]["plot-player-tracking"].as<int>() == 0 &&
          mapped_defaults["ds-playtracker"]["draw"].as<int>() == 0 &&
          mapped_defaults["ds-fieldmask"]["properties"]["raise-bbox-center-by-height-ratio"].as<double>() == -0.1 &&
          mapped_defaults["ds-fieldmask"]["properties"]["lower-bbox-bottom-by-height-ratio"].as<double>() == 0.1 &&
          mapped_defaults["sink0"]["bitrate"].as<int>() == 55000000 &&
          !mapped_defaults["sink0"]["output-file"].IsDefined() && !mapped_defaults["sink0"]["width"].IsDefined() &&
          !mapped_defaults["sink0"]["height"].IsDefined() &&
          mapped_defaults["hmplaycropper"]["fixed-edge-rotation-angle"].as<double>() == 10.0 &&
          mapped_defaults["ds-playtracker"]["fixed-edge-rotation-angle"].as<double>() == 10.0 &&
          mapped_defaults["hmplaycropper"]["scoreboard-projected-width"].as<std::string>() == "%10" &&
          mapped_defaults["hmplaycropper"]["scoreboard-projected-height"].as<std::string>() == "%20" &&
          mapped_defaults["hmplaycropper"]["scoreboard-scale"].as<double>() == 1.0 &&
          !mapped_defaults["hmplaycropper"]["scoreboard-perspective-polygon"].IsDefined(),
      "Every supported non-tracker native default must be filled from the bundled canonical baseline");

  const fs::path primary_draw_baseline_root = root / "primary-draw-baseline";
  fs::create_directories(primary_draw_baseline_root);
  YAML::Node primary_draw_baseline = YAML::Clone(test_baseline);
  primary_draw_baseline["plot"]["debug_play_tracker"] = true;
  std::ofstream(primary_draw_baseline_root / "baseline.yaml") << YAML::Dump(primary_draw_baseline) << '\n';
  fs::create_directories(games / "mapping-primary-app");
  hm::Configurator mapping_primary_app(
      "mapping-primary-app", primary_draw_baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const fs::path primary_app_config =
      bundled_baseline.ok() ? bundled_baseline->root / "ds_hockey_app_config.yaml" : fs::path();
  const bool mapping_primary_loaded = bundled_baseline.ok() && fs::is_regular_file(primary_app_config) &&
      mapping_primary_app.configure().ok() &&
      mapping_primary_app.underlay_config("pipeline", primary_app_config.string());
  const absl::Status mapping_primary_status = mapping_primary_loaded
      ? mapping_primary_app.apply_supported_baseline_mappings()
      : absl::InternalError("primary app mapping fixture did not load");
  ok &= expect(
      mapping_primary_status.ok() &&
          mapping_primary_app.config()["pipeline"]["ds-playtracker"]["draw"].as<int>() == 1 &&
          mapping_primary_app.config()["pipeline"]["hmplaycropper"]["plot-play-tracking"].as<int>() == 1 &&
          mapping_primary_app.config()["pipeline"]["hmplaycropper"]["plot-player-tracking"].as<int>() == 1,
      "The primary app config must derive every native debug plot control from the canonical baseline");

  const fs::path structural_custom_path = root / "mapping-structural-custom.yaml";
  YAML::Node structural_custom = YAML::Clone(mapping_structure);
  structural_custom["application"]["video-converter"] = "dsxvideoconvert";
  structural_custom["hmstitcher"]["enable"] = 0;
  structural_custom["hmstitcher"]["minimize-blend"] = 1;
  structural_custom["hmstitcher"]["stitch-compute-precision"] = "fp16";
  structural_custom["hmstitcher"]["post_stitch_rotate_degrees"] = 37.0;
  structural_custom["hmplaycropper"]["no-crop"] = 1;
  structural_custom["hmplaycropper"]["plot-play-tracking"] = 1;
  structural_custom["hmplaycropper"]["plot-player-tracking"] = 1;
  structural_custom["ds-playtracker"]["draw"] = 1;
  structural_custom["ds-fieldmask"]["properties"]["raise-bbox-center-by-height-ratio"] = -0.25;
  structural_custom["ds-fieldmask"]["properties"]["lower-bbox-bottom-by-height-ratio"] = 0.35;
  structural_custom["hmplaycropper"]["fixed-edge-rotation-angle"] = 12.0;
  structural_custom["hmplaycropper"]["scoreboard-projected-width"] = "%77";
  structural_custom["sink0"]["bitrate"] = 999999;
  structural_custom["sink0"]["output-file"] = "structural.mkv";
  structural_custom["sink0"]["width"] = 999;
  std::ofstream(structural_custom_path) << YAML::Dump(structural_custom) << '\n';
  fs::create_directories(games / "mapping-structural");
  hm::Configurator mapping_structural(
      "mapping-structural", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_structural_loaded = mapping_structural.configure().ok() &&
      mapping_structural.underlay_config("pipeline", structural_custom_path.string());
  const absl::Status mapping_structural_status = mapping_structural_loaded
      ? mapping_structural.apply_supported_baseline_mappings()
      : absl::InternalError("mapping structural fixture did not load");
  const YAML::Node mapped_structural = mapping_structural.config()["pipeline"];
  ok &= expect(
      mapping_structural_status.ok() && mapped_structural["hmstitcher"]["enable"].as<int>() == 0 &&
          mapped_structural["application"]["video-converter"].as<std::string>() == "dsxvideoconvert" &&
          mapped_structural["hmstitcher"]["minimize-blend"].as<int>() == 1 &&
          mapped_structural["hmstitcher"]["stitch-compute-precision"].as<std::string>() == "fp16" &&
          mapped_structural["hmstitcher"]["post-stitch-rotate-degrees"].as<double>() == 37.0 &&
          mapped_structural["hmplaycropper"]["no-crop"].as<int>() == 1 &&
          mapped_structural["hmplaycropper"]["plot-play-tracking"].as<int>() == 1 &&
          mapped_structural["hmplaycropper"]["plot-player-tracking"].as<int>() == 1 &&
          mapped_structural["ds-playtracker"]["draw"].as<int>() == 1 &&
          mapped_structural["ds-fieldmask"]["properties"]["raise-bbox-center-by-height-ratio"].as<double>() == -0.25 &&
          mapped_structural["ds-fieldmask"]["properties"]["lower-bbox-bottom-by-height-ratio"].as<double>() == 0.35 &&
          mapped_structural["hmplaycropper"]["fixed-edge-rotation-angle"].as<double>() == 12.0 &&
          mapped_structural["hmplaycropper"]["scoreboard-projected-width"].as<std::string>() == "%77" &&
          mapped_structural["sink0"]["bitrate"].as<int>() == 999999 &&
          mapped_structural["sink0"]["output-file"].as<std::string>() == "structural.mkv" &&
          mapped_structural["sink0"]["width"].as<int>() == 999,
      "Bundled defaults must fill omissions without replacing custom structural native values");

  const fs::path no_application_structure_path = root / "mapping-no-application-structure.yaml";
  YAML::Node no_application_structure = YAML::Clone(mapping_structure);
  no_application_structure.remove("application");
  std::ofstream(no_application_structure_path) << YAML::Dump(no_application_structure) << '\n';
  const fs::path runtime_video_converter_game_dir = games / "mapping-runtime-video-converter";
  fs::create_directories(runtime_video_converter_game_dir);
  YAML::Node runtime_video_converter_override(YAML::NodeType::Map);
  runtime_video_converter_override["runtime"]["video_converter"] = "dsxvideoconvert";
  std::ofstream(runtime_video_converter_game_dir / "config.yaml")
      << YAML::Dump(runtime_video_converter_override) << '\n';
  hm::Configurator mapping_runtime_video_converter(
      "mapping-runtime-video-converter", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_runtime_video_converter_loaded = mapping_runtime_video_converter.configure().ok() &&
      mapping_runtime_video_converter.underlay_config("pipeline", no_application_structure_path.string());
  const absl::Status mapping_runtime_video_converter_status = mapping_runtime_video_converter_loaded
      ? mapping_runtime_video_converter.apply_supported_baseline_mappings()
      : absl::InternalError("mapping runtime video-converter fixture did not load");
  ok &= expect(
      mapping_runtime_video_converter_status.ok() &&
          mapping_runtime_video_converter.config()["pipeline"]["application"]["video-converter"].as<std::string>() ==
              "dsxvideoconvert",
      "Canonical runtime.video_converter must create application.video-converter when application is absent");

  const fs::path canonical_game_dir = games / "mapping-canonical";
  fs::create_directories(canonical_game_dir);
  YAML::Node canonical_overrides(YAML::NodeType::Map);
  canonical_overrides["runtime"]["video_converter"] = "nvvideoconvert";
  canonical_overrides["stitching"]["enabled"] = false;
  canonical_overrides["stitching"]["minimize_blend"] = true;
  canonical_overrides["stitching"]["max_output_width"] = 4096;
  canonical_overrides["stitching"]["dtype"] = "float16";
  canonical_overrides["stitching"]["post_stitch_rotate_degrees"] = 15.0;
  canonical_overrides["apply_camera"]["crop_output_image"] = false;
  canonical_overrides["rink"]["camera"]["fixed_edge_rotation_angle"].push_back(21.0);
  canonical_overrides["rink"]["camera"]["fixed_edge_rotation_angle"].push_back(22.0);
  canonical_overrides["rink"]["scoreboard"]["projected_width"] = "%11";
  canonical_overrides["rink"]["scoreboard"]["projected_height"] = "%22";
  canonical_overrides["rink"]["scoreboard"]["scoreboard_scale"] = 1.25;
  for (const auto& point : std::vector<std::pair<int, int>>{{1, 2}, {3, 4}, {5, 6}, {7, 8}}) {
    YAML::Node coordinates(YAML::NodeType::Sequence);
    coordinates.push_back(point.first);
    coordinates.push_back(point.second);
    canonical_overrides["rink"]["scoreboard"]["perspective_polygon"].push_back(coordinates);
  }
  canonical_overrides["plot"]["plot_moving_boxes"] = true;
  canonical_overrides["plot"]["plot_individual_player_tracking"] = true;
  canonical_overrides["plot"]["debug_play_tracker"] = false;
  canonical_overrides["ice_boundaries"]["raise_bbox_center_by_height_ratio"] = -0.4;
  canonical_overrides["ice_boundaries"]["lower_bbox_bottom_by_height_ratio"] = 0.45;
  canonical_overrides["video_out"]["bit_rate"] = 123456;
  canonical_overrides["video_out"]["output_video_path"] = "/tmp/canonical.mkv";
  canonical_overrides["video_out"]["output_width"] = 1280;
  canonical_overrides["video_out"]["output_height"] = 720;
  std::ofstream(canonical_game_dir / "config.yaml") << YAML::Dump(canonical_overrides) << '\n';
  hm::Configurator mapping_canonical("mapping-canonical", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_canonical_loaded = mapping_canonical.configure().ok() &&
      mapping_canonical.underlay_config("pipeline", structural_custom_path.string());
  const absl::Status mapping_canonical_status = mapping_canonical_loaded
      ? mapping_canonical.apply_supported_baseline_mappings()
      : absl::InternalError("mapping canonical fixture did not load");
  const YAML::Node mapped_canonical = mapping_canonical.config()["pipeline"];
  ok &= expect(
      mapping_canonical_status.ok() && mapped_canonical["hmstitcher"]["enable"].as<int>() == 0 &&
          mapped_canonical["application"]["video-converter"].as<std::string>() == "nvvideoconvert" &&
          mapped_canonical["hmstitcher"]["minimize-blend"].as<int>() == 1 &&
          mapped_canonical["hmstitcher"]["properties"]["max-output-width"].as<int>() == 4096 &&
          mapped_canonical["hmstitcher"]["stitch-compute-precision"].as<std::string>() == "fp16" &&
          mapped_canonical["hmstitcher"]["post-stitch-rotate-degrees"].as<double>() == 15.0 &&
          mapped_canonical["hmplaycropper"]["no-crop"].as<int>() == 1 &&
          mapped_canonical["hmplaycropper"]["plot-play-tracking"].as<int>() == 1 &&
          mapped_canonical["hmplaycropper"]["plot-player-tracking"].as<int>() == 1 &&
          mapped_canonical["ds-playtracker"]["draw"].as<int>() == 1 &&
          mapped_canonical["ds-fieldmask"]["properties"]["raise-bbox-center-by-height-ratio"].as<double>() == -0.4 &&
          mapped_canonical["ds-fieldmask"]["properties"]["lower-bbox-bottom-by-height-ratio"].as<double>() == 0.45 &&
          !mapped_canonical["hmplaycropper"]["fixed-edge-rotation-angle"].IsDefined() &&
          mapped_canonical["hmplaycropper"]["fixed-edge-rotation-angle-left"].as<double>() == 21.0 &&
          mapped_canonical["hmplaycropper"]["fixed-edge-rotation-angle-right"].as<double>() == 22.0 &&
          mapped_canonical["ds-playtracker"]["fixed-edge-rotation-angle-left"].as<double>() == 21.0 &&
          mapped_canonical["ds-playtracker"]["fixed-edge-rotation-angle-right"].as<double>() == 22.0 &&
          mapped_canonical["hmplaycropper"]["scoreboard-projected-width"].as<std::string>() == "%11" &&
          mapped_canonical["hmplaycropper"]["scoreboard-projected-height"].as<std::string>() == "%22" &&
          mapped_canonical["hmplaycropper"]["scoreboard-scale"].as<double>() == 1.25 &&
          mapped_canonical["hmplaycropper"]["scoreboard-perspective-polygon"].as<std::string>() == "1,2,3,4,5,6,7,8" &&
          mapped_canonical["sink0"]["bitrate"].as<int>() == 123456 &&
          mapped_canonical["sink0"]["output-file"].as<std::string>() == "/tmp/canonical.mkv" &&
          mapped_canonical["sink0"]["width"].as<int>() == 1280 && mapped_canonical["sink0"]["height"].as<int>() == 720,
      "Explicit canonical game values must replace lower-ranked structural native values for every supported mapping");

  const fs::path stale_runtime_polygon_dir = games / "mapping-stale-runtime-polygon";
  fs::create_directories(stale_runtime_polygon_dir);
  YAML::Node stale_runtime_polygon(YAML::NodeType::Map);
  stale_runtime_polygon["stitching"]["enabled"] = true;
  stale_runtime_polygon["stitching"]["post_stitch_rotate_degrees"] = 15.0;
  stale_runtime_polygon["stitching"]["generated_field_mask_post_stitch_rotate_degrees"] = 0.0;
  for (const auto& point : std::vector<std::pair<int, int>>{{1, 2}, {3, 4}, {5, 6}, {7, 8}}) {
    YAML::Node coordinates(YAML::NodeType::Sequence);
    coordinates.push_back(point.first);
    coordinates.push_back(point.second);
    stale_runtime_polygon["rink"]["scoreboard"]["perspective_polygon"].push_back(coordinates);
  }
  std::ofstream(stale_runtime_polygon_dir / "config.yaml") << YAML::Dump(stale_runtime_polygon) << '\n';
  const fs::path stale_runtime_structure_path = root / "mapping-stale-runtime-structure.yaml";
  YAML::Node stale_runtime_structure = YAML::Clone(structural_custom);
  stale_runtime_structure["application"]["complete-configuration"] = 1;
  std::ofstream(stale_runtime_structure_path) << YAML::Dump(stale_runtime_structure) << '\n';
  hm::Configurator stale_runtime_polygon_configurator(
      "mapping-stale-runtime-polygon", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool stale_runtime_polygon_loaded = stale_runtime_polygon_configurator.configure().ok() &&
      stale_runtime_polygon_configurator.underlay_config("pipeline", stale_runtime_structure_path.string());
  const absl::Status stale_runtime_polygon_status = stale_runtime_polygon_loaded
      ? stale_runtime_polygon_configurator.complete_configuration(
            /*force=*/false,
            /*clean_stitching_artifacts=*/false,
            /*clean_stitching_from_control_points=*/false,
            /*clean_expected_invalidation_id=*/{},
            /*show_render_sink=*/false,
            /*show_render_scale=*/-1.0,
            bundled_baseline.ok() ? bundled_baseline->root : fs::path())
      : absl::InternalError("stale runtime scoreboard fixture did not load");
  const YAML::Node stale_runtime_after = fs::is_regular_file(stale_runtime_polygon_dir / "config.yaml")
      ? YAML::LoadFile((stale_runtime_polygon_dir / "config.yaml").string())
      : YAML::Node();
  const auto stale_runtime_rotation_marker =
      hm::get_node(stale_runtime_after, "stitching.generated_field_mask_post_stitch_rotate_degrees");
  const auto stale_runtime_native_polygon = hm::get_node(
      stale_runtime_polygon_configurator.config(), "pipeline.hmplaycropper.scoreboard-perspective-polygon");
  const auto stale_runtime_persisted_polygon = hm::get_node(stale_runtime_after, "rink.scoreboard.perspective_polygon");
  const bool stale_runtime_polygon_cleared = stale_runtime_polygon_loaded &&
      !stale_runtime_native_polygon.has_value() && !stale_runtime_persisted_polygon.has_value() &&
      stale_runtime_rotation_marker.has_value() && stale_runtime_rotation_marker->IsScalar() &&
      stale_runtime_rotation_marker->as<double>() == 15.0;
  if (!stale_runtime_polygon_cleared) {
    std::cerr << "stale runtime polygon configuration status: " << stale_runtime_polygon_status << '\n'
              << "runtime config: " << YAML::Dump(stale_runtime_polygon_configurator.config()) << '\n'
              << "private config: " << YAML::Dump(stale_runtime_after) << '\n';
  }
  ok &= expect(
      stale_runtime_polygon_cleared,
      "Rotation invalidation must clear a canonical polygon already materialized into the active native pipeline");

  const fs::path canonical_null_game_dir = games / "mapping-canonical-null";
  fs::create_directories(canonical_null_game_dir);
  YAML::Node native_width(YAML::NodeType::Map);
  native_width["hmstitcher"]["properties"]["max-output-width"] = 2048;
  native_width["hmstitcher"]["private-properties"]["max-output-width"] = 2048;
  native_width["hmstitcher"]["private-properties"]["max_output_width"] = 2048;
  native_width["hmstitcher"]["private-properties"]["stitch-max-output-width"] = 2048;
  native_width["hmstitcher"]["private-properties"]["stitch_max_output_width"] = 2048;
  std::ofstream(root / "mapping-native-width.yaml") << YAML::Dump(native_width) << '\n';
  YAML::Node canonical_null(YAML::NodeType::Map);
  canonical_null["stitching"]["max_output_width"] = YAML::Node(YAML::NodeType::Null);
  std::ofstream(canonical_null_game_dir / "config.yaml") << YAML::Dump(canonical_null) << '\n';
  hm::Configurator mapping_canonical_null(
      "mapping-canonical-null", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_canonical_null_loaded = mapping_canonical_null.configure().ok() &&
      mapping_canonical_null.underlay_config("pipeline", (root / "mapping-native-width.yaml").string());
  const absl::Status mapping_canonical_null_status = mapping_canonical_null_loaded
      ? mapping_canonical_null.apply_supported_baseline_mappings()
      : absl::InternalError("mapping canonical-null fixture did not load");
  ok &= expect(
      mapping_canonical_null_status.ok() &&
          !mapping_canonical_null.config()["pipeline"]["hmstitcher"]["properties"]["max-output-width"].IsDefined() &&
          !mapping_canonical_null.config()["pipeline"]["hmstitcher"]["private-properties"]["max-output-width"]
               .IsDefined() &&
          !mapping_canonical_null.config()["pipeline"]["hmstitcher"]["private-properties"]["max_output_width"]
               .IsDefined() &&
          !mapping_canonical_null.config()["pipeline"]["hmstitcher"]["private-properties"]["stitch-max-output-width"]
               .IsDefined() &&
          !mapping_canonical_null.config()["pipeline"]["hmstitcher"]["private-properties"]["stitch_max_output_width"]
               .IsDefined(),
      "An explicit canonical null max stitched width must clear lower-ranked native caps");

  const fs::path private_only_width_game_dir = games / "mapping-private-only-width";
  fs::create_directories(private_only_width_game_dir);
  YAML::Node private_only_width(YAML::NodeType::Map);
  private_only_width["hmstitcher"]["private-properties"]["stitch_max_output_width"] = 1536;
  std::ofstream(root / "mapping-private-only-width.yaml") << YAML::Dump(private_only_width) << '\n';
  hm::Configurator mapping_private_only_width(
      "mapping-private-only-width", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_private_only_width_loaded = mapping_private_only_width.configure().ok() &&
      mapping_private_only_width.underlay_config("pipeline", (root / "mapping-private-only-width.yaml").string());
  const absl::Status mapping_private_only_width_status = mapping_private_only_width_loaded
      ? mapping_private_only_width.apply_supported_baseline_mappings()
      : absl::InternalError("mapping private-only-width fixture did not load");
  ok &= expect(
      mapping_private_only_width_status.ok() &&
          !mapping_private_only_width.config()["pipeline"]["hmstitcher"]["properties"]["max-output-width"]
               .IsDefined() &&
          mapping_private_only_width.config()["pipeline"]["hmstitcher"]["private-properties"]["stitch_max_output_width"]
                  .as<int>() == 1536,
      "Bundled baseline null max stitched width must not clear a private-only native cap");

  const fs::path private_width_public_null_game_dir = games / "mapping-private-width-public-null";
  fs::create_directories(private_width_public_null_game_dir);
  YAML::Node private_width_public_null(YAML::NodeType::Map);
  private_width_public_null["hmstitcher"]["properties"]["max-output-width"] = YAML::Node(YAML::NodeType::Null);
  private_width_public_null["hmstitcher"]["private-properties"]["stitch_max_output_width"] = 1536;
  const fs::path private_width_public_null_path = root / "mapping-private-width-public-null.yaml";
  std::ofstream(private_width_public_null_path) << YAML::Dump(private_width_public_null) << '\n';
  hm::Configurator mapping_private_width_public_null(
      "mapping-private-width-public-null", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_private_width_public_null_loaded = mapping_private_width_public_null.configure().ok() &&
      mapping_private_width_public_null.underlay_config("pipeline", private_width_public_null_path.string());
  const absl::Status mapping_private_width_public_null_status = mapping_private_width_public_null_loaded
      ? mapping_private_width_public_null.apply_supported_baseline_mappings()
      : absl::InternalError("mapping private-width/public-null fixture did not load");
  ok &= expect(
      mapping_private_width_public_null_status.ok() &&
          !mapping_private_width_public_null.config()["pipeline"]["hmstitcher"]["properties"]["max-output-width"]
               .IsDefined() &&
          mapping_private_width_public_null
                  .config()["pipeline"]["hmstitcher"]["private-properties"]["stitch_max_output_width"]
                  .as<int>() == 1536,
      "An unranked private max-width cap must remove a same-rank public null alias");

  const fs::path conflicting_private_width_game_dir = games / "mapping-conflicting-private-width";
  fs::create_directories(conflicting_private_width_game_dir);
  YAML::Node conflicting_private_width(YAML::NodeType::Map);
  conflicting_private_width["hmstitcher"]["private-properties"]["max-output-width"] = 1536;
  conflicting_private_width["hmstitcher"]["private-properties"]["stitch_max_output_width"] = 3072;
  const fs::path conflicting_private_width_path = root / "mapping-conflicting-private-width.yaml";
  std::ofstream(conflicting_private_width_path) << YAML::Dump(conflicting_private_width) << '\n';
  hm::Configurator mapping_conflicting_private_width(
      "mapping-conflicting-private-width", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_conflicting_private_width_loaded = mapping_conflicting_private_width.configure().ok() &&
      mapping_conflicting_private_width.underlay_config("pipeline", conflicting_private_width_path.string());
  const absl::Status mapping_conflicting_private_width_status = mapping_conflicting_private_width_loaded
      ? mapping_conflicting_private_width.apply_supported_baseline_mappings()
      : absl::InternalError("mapping conflicting-private-width fixture did not load");
  ok &= expect(
      mapping_conflicting_private_width_status.ok() &&
          mapping_conflicting_private_width.config()["pipeline"]["hmstitcher"]["private-properties"]["max-output-width"]
                  .as<int>() == 1536 &&
          !mapping_conflicting_private_width
               .config()["pipeline"]["hmstitcher"]["private-properties"]["stitch_max_output_width"]
               .IsDefined(),
      "Conflicting unranked private max-width aliases must normalize to the deterministic winner");

  const fs::path native_null_width_game_dir = games / "mapping-native-null-width";
  fs::create_directories(native_null_width_game_dir);
  YAML::Node native_null_width(YAML::NodeType::Map);
  native_null_width["hmstitcher"]["properties"]["max-output-width"] = YAML::Node(YAML::NodeType::Null);
  native_null_width["hmstitcher"]["private-properties"]["stitch_max_output_width"] = YAML::Node(YAML::NodeType::Null);
  const fs::path native_null_width_path = root / "mapping-native-null-width.yaml";
  std::ofstream(native_null_width_path) << YAML::Dump(native_null_width) << '\n';
  hm::Configurator mapping_native_null_width(
      "mapping-native-null-width", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_native_null_width_loaded = mapping_native_null_width.configure().ok() &&
      mapping_native_null_width.underlay_config("pipeline", native_null_width_path.string());
  const absl::Status mapping_native_null_width_status = mapping_native_null_width_loaded
      ? mapping_native_null_width.apply_supported_baseline_mappings()
      : absl::InternalError("mapping native-null-width fixture did not load");
  ok &= expect(
      mapping_native_null_width_status.ok() &&
          !mapping_native_null_width.config()["pipeline"]["hmstitcher"]["properties"]["max-output-width"].IsDefined() &&
          !mapping_native_null_width.config()["pipeline"]["hmstitcher"]["private-properties"]["stitch_max_output_width"]
               .IsDefined(),
      "Unranked null native max-width aliases must normalize to an absent GObject property");

  const fs::path ranked_width_game_dir = games / "mapping-ranked-max-width-alias";
  fs::create_directories(ranked_width_game_dir);
  YAML::Node lower_ranked_canonical(YAML::NodeType::Map);
  lower_ranked_canonical["stitching"]["max_output_width"] = 1024;
  std::ofstream(ranked_width_game_dir / "config.yaml") << YAML::Dump(lower_ranked_canonical) << '\n';
  hm::Configurator mapping_ranked_width(
      "mapping-ranked-max-width-alias", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_ranked_width_loaded = mapping_ranked_width.configure().ok() &&
      mapping_ranked_width.apply_config_item("pipeline.hmstitcher.properties.stitch_max_output_width", "3072").ok();
  const absl::Status mapping_ranked_width_status = mapping_ranked_width_loaded
      ? mapping_ranked_width.apply_supported_baseline_mappings()
      : absl::InternalError("mapping ranked-width fixture did not load");
  ok &= expect(
      mapping_ranked_width_status.ok() &&
          mapping_ranked_width.config()["pipeline"]["hmstitcher"]["properties"]["max-output-width"].as<int>() == 3072 &&
          !mapping_ranked_width.config()["pipeline"]["hmstitcher"]["properties"]["stitch_max_output_width"].IsDefined(),
      "A higher-ranked native max-width alias must beat a lower-ranked canonical value and normalize to max-output-width");

  const fs::path max_width_tie_game_dir = games / "mapping-max-width-tie";
  fs::create_directories(max_width_tie_game_dir);
  YAML::Node max_width_tie(YAML::NodeType::Map);
  max_width_tie["stitching"]["max_output_width"] = 4096;
  max_width_tie["pipeline"]["hmstitcher"]["properties"]["max-output-width"] = 2048;
  std::ofstream(max_width_tie_game_dir / "config.yaml") << YAML::Dump(max_width_tie) << '\n';
  hm::Configurator mapping_max_width_tie(
      "mapping-max-width-tie", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status mapping_max_width_tie_status = mapping_max_width_tie.configure().ok()
      ? mapping_max_width_tie.apply_supported_baseline_mappings()
      : absl::InternalError("mapping max-width-tie fixture did not load");
  ok &= expect(
      mapping_max_width_tie_status.ok() &&
          mapping_max_width_tie.config()["pipeline"]["hmstitcher"]["properties"]["max-output-width"].as<int>() == 4096,
      "A same-ranked canonical max stitched width must match UI precedence and beat conflicting native aliases");

  const fs::path malformed_properties_game_dir = games / "mapping-malformed-properties";
  fs::create_directories(malformed_properties_game_dir);
  YAML::Node malformed_properties(YAML::NodeType::Map);
  malformed_properties["hmstitcher"]["properties"] = "not-a-map";
  std::ofstream(root / "mapping-malformed-properties.yaml") << YAML::Dump(malformed_properties) << '\n';
  hm::Configurator mapping_malformed_properties(
      "mapping-malformed-properties", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_malformed_properties_loaded = mapping_malformed_properties.configure().ok() &&
      mapping_malformed_properties.underlay_config("pipeline", (root / "mapping-malformed-properties.yaml").string());
  const absl::Status mapping_malformed_properties_status = mapping_malformed_properties_loaded
      ? mapping_malformed_properties.apply_supported_baseline_mappings()
      : absl::InternalError("mapping malformed-properties fixture did not load");
  ok &= expect(
      absl::IsInvalidArgument(mapping_malformed_properties_status),
      "A malformed native hmstitcher properties node must be rejected instead of replaced");

  const fs::path public_alias_game_dir = games / "mapping-public-max-width-alias";
  fs::create_directories(public_alias_game_dir);
  YAML::Node public_alias(YAML::NodeType::Map);
  public_alias["hmstitcher"]["properties"]["stitch_max_output_width"] = 3072;
  std::ofstream(root / "mapping-public-max-width-alias.yaml") << YAML::Dump(public_alias) << '\n';
  hm::Configurator mapping_public_alias(
      "mapping-public-max-width-alias", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_public_alias_loaded = mapping_public_alias.configure().ok() &&
      mapping_public_alias.underlay_config("pipeline", (root / "mapping-public-max-width-alias.yaml").string());
  const absl::Status mapping_public_alias_status = mapping_public_alias_loaded
      ? mapping_public_alias.apply_supported_baseline_mappings()
      : absl::InternalError("mapping public-alias fixture did not load");
  ok &= expect(
      mapping_public_alias_status.ok() &&
          mapping_public_alias.config()["pipeline"]["hmstitcher"]["properties"]["max-output-width"].as<int>() == 3072 &&
          !mapping_public_alias.config()["pipeline"]["hmstitcher"]["properties"]["stitch_max_output_width"].IsDefined(),
      "A public hmstitcher max-width alias must normalize to the typed GObject property");

  ok &= expect(
      mapping_canonical.apply_config_item("pipeline.hmstitcher.enable", "1").ok() &&
          mapping_canonical.apply_supported_baseline_mappings().ok() &&
          mapping_canonical.config()["pipeline"]["hmstitcher"]["enable"].as<int>() == 1 &&
          mapping_canonical.apply_config_item("stitching.enabled", "false").ok() &&
          mapping_canonical.apply_supported_baseline_mappings().ok() &&
          mapping_canonical.config()["pipeline"]["hmstitcher"]["enable"].as<int>() == 1,
      "A higher-ranked direct native value must win, and direct native must win a same-rank canonical tie");

  fs::create_directories(games / "mapping-video-converter-cli");
  hm::Configurator mapping_video_converter_cli(
      "mapping-video-converter-cli", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_video_converter_cli_loaded = mapping_video_converter_cli.configure().ok();
  const absl::Status video_converter_cli_canonical =
      mapping_video_converter_cli.apply_config_item("runtime.video_converter", "dsxvideoconvert");
  const absl::Status video_converter_cli_canonical_dashed =
      mapping_video_converter_cli.apply_config_item("runtime.video-converter", "dsxvideoconvert");
  const absl::Status video_converter_cli_native =
      mapping_video_converter_cli.apply_config_item("pipeline.application.video-converter", "dsxvideoconvert");
  const absl::Status video_converter_cli_native_underscored =
      mapping_video_converter_cli.apply_config_item("pipeline.application.video_converter", "dsxvideoconvert");
  ok &= expect(
      mapping_video_converter_cli_loaded && absl::IsInvalidArgument(video_converter_cli_canonical) &&
          absl::IsInvalidArgument(video_converter_cli_canonical_dashed) &&
          absl::IsInvalidArgument(video_converter_cli_native) &&
          absl::IsInvalidArgument(video_converter_cli_native_underscored),
      "Video converter selection must not be configurable through CLI-style options");

  const fs::path fixed_edge_same_rank_game_dir = games / "mapping-fixed-edge-same-rank";
  fs::create_directories(fixed_edge_same_rank_game_dir);
  YAML::Node fixed_edge_same_rank(YAML::NodeType::Map);
  fixed_edge_same_rank["rink"]["camera"]["fixed_edge_rotation_angle"].push_back(21.0);
  fixed_edge_same_rank["rink"]["camera"]["fixed_edge_rotation_angle"].push_back(22.0);
  fixed_edge_same_rank["pipeline"]["hmplaycropper"]["fixed-edge-rotation-angle"] = 12.0;
  fixed_edge_same_rank["pipeline"]["ds-playtracker"]["fixed-edge-rotation-angle"] = 13.0;
  std::ofstream(fixed_edge_same_rank_game_dir / "config.yaml") << YAML::Dump(fixed_edge_same_rank) << '\n';
  hm::Configurator mapping_fixed_edge_same_rank(
      "mapping-fixed-edge-same-rank", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_fixed_edge_same_rank_loaded = mapping_fixed_edge_same_rank.configure().ok() &&
      mapping_fixed_edge_same_rank.underlay_config("pipeline", mapping_structure_path.string());
  const absl::Status mapping_fixed_edge_same_rank_status = mapping_fixed_edge_same_rank_loaded
      ? mapping_fixed_edge_same_rank.apply_supported_baseline_mappings()
      : absl::InternalError("fixed-edge same-rank fixture did not load");
  const YAML::Node mapped_fixed_edge_same_rank = mapping_fixed_edge_same_rank.config()["pipeline"];
  ok &= expect(
      mapping_fixed_edge_same_rank_status.ok() &&
          mapped_fixed_edge_same_rank["hmplaycropper"]["fixed-edge-rotation-angle"].as<double>() == 12.0 &&
          !mapped_fixed_edge_same_rank["hmplaycropper"]["fixed-edge-rotation-angle-left"].IsDefined() &&
          !mapped_fixed_edge_same_rank["hmplaycropper"]["fixed-edge-rotation-angle-right"].IsDefined() &&
          mapped_fixed_edge_same_rank["ds-playtracker"]["fixed-edge-rotation-angle"].as<double>() == 13.0 &&
          !mapped_fixed_edge_same_rank["ds-playtracker"]["fixed-edge-rotation-angle-left"].IsDefined() &&
          !mapped_fixed_edge_same_rank["ds-playtracker"]["fixed-edge-rotation-angle-right"].IsDefined(),
      "A same-rank direct native fixed-edge angle must own both sides over a canonical array");

  user_overlay["rink"]["camera"]["fixed_edge_rotation_angle"].push_back(31.0);
  user_overlay["rink"]["camera"]["fixed_edge_rotation_angle"].push_back(32.0);
  std::ofstream(user_config_path) << YAML::Dump(user_overlay) << '\n';
  const fs::path fixed_edge_higher_native_game_dir = games / "mapping-fixed-edge-higher-native";
  fs::create_directories(fixed_edge_higher_native_game_dir);
  YAML::Node fixed_edge_higher_native(YAML::NodeType::Map);
  fixed_edge_higher_native["pipeline"]["hmplaycropper"]["fixed-edge-rotation-angle"] = 14.0;
  fixed_edge_higher_native["pipeline"]["ds-playtracker"]["fixed-edge-rotation-angle"] = 15.0;
  std::ofstream(fixed_edge_higher_native_game_dir / "config.yaml") << YAML::Dump(fixed_edge_higher_native) << '\n';
  hm::Configurator mapping_fixed_edge_higher_native(
      "mapping-fixed-edge-higher-native", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_fixed_edge_higher_native_loaded = mapping_fixed_edge_higher_native.configure().ok() &&
      mapping_fixed_edge_higher_native.underlay_config("pipeline", mapping_structure_path.string());
  const absl::Status mapping_fixed_edge_higher_native_status = mapping_fixed_edge_higher_native_loaded
      ? mapping_fixed_edge_higher_native.apply_supported_baseline_mappings()
      : absl::InternalError("fixed-edge higher-native fixture did not load");
  const YAML::Node mapped_fixed_edge_higher_native = mapping_fixed_edge_higher_native.config()["pipeline"];
  ok &= expect(
      mapping_fixed_edge_higher_native_status.ok() &&
          mapped_fixed_edge_higher_native["hmplaycropper"]["fixed-edge-rotation-angle"].as<double>() == 14.0 &&
          !mapped_fixed_edge_higher_native["hmplaycropper"]["fixed-edge-rotation-angle-left"].IsDefined() &&
          !mapped_fixed_edge_higher_native["hmplaycropper"]["fixed-edge-rotation-angle-right"].IsDefined() &&
          mapped_fixed_edge_higher_native["ds-playtracker"]["fixed-edge-rotation-angle"].as<double>() == 15.0 &&
          !mapped_fixed_edge_higher_native["ds-playtracker"]["fixed-edge-rotation-angle-left"].IsDefined() &&
          !mapped_fixed_edge_higher_native["ds-playtracker"]["fixed-edge-rotation-angle-right"].IsDefined(),
      "A higher-ranked direct native fixed-edge angle must own both sides over a lower canonical array");
  user_overlay["rink"]["camera"].remove("fixed_edge_rotation_angle");
  std::ofstream(user_config_path) << YAML::Dump(user_overlay) << '\n';

  const fs::path native_game_dir = games / "mapping-native-precedence";
  fs::create_directories(native_game_dir);
  YAML::Node native_game_override(YAML::NodeType::Map);
  native_game_override["pipeline"]["hmstitcher"]["enable"] = 1;
  std::ofstream(native_game_dir / "config.yaml") << YAML::Dump(native_game_override) << '\n';
  hm::Configurator mapping_native_precedence(
      "mapping-native-precedence", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_native_precedence_loaded = mapping_native_precedence.configure().ok() &&
      mapping_native_precedence.underlay_config("pipeline", mapping_structure_path.string()) &&
      mapping_native_precedence.apply_config_item("stitching.enabled", "false").ok();
  const absl::Status mapping_native_precedence_status = mapping_native_precedence_loaded
      ? mapping_native_precedence.apply_supported_baseline_mappings()
      : absl::InternalError("mapping native precedence fixture did not load");
  ok &= expect(
      mapping_native_precedence_status.ok() &&
          mapping_native_precedence.config()["pipeline"]["hmstitcher"]["enable"].as<int>() == 0,
      "A higher-ranked canonical CLI value must replace a lower-ranked direct native game value");

  fs::create_directories(games / "mapping-plot-or");
  hm::Configurator mapping_plot_or("mapping-plot-or", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_plot_or_loaded = mapping_plot_or.configure().ok() &&
      mapping_plot_or.underlay_config("pipeline", mapping_structure_path.string()) &&
      mapping_plot_or.apply_config_item("plot.debug_play_tracker", "false").ok() &&
      mapping_plot_or.apply_config_item("plot.plot_moving_boxes", "true").ok() &&
      mapping_plot_or.apply_config_item("plot.plot_individual_player_tracking", "false").ok();
  const absl::Status mapping_plot_or_status = mapping_plot_or_loaded
      ? mapping_plot_or.apply_supported_baseline_mappings()
      : absl::InternalError("plot OR mapping fixture did not load");
  ok &= expect(
      mapping_plot_or_status.ok() && mapping_plot_or.config()["pipeline"]["ds-playtracker"]["draw"].as<int>() == 1 &&
          mapping_plot_or.config()["pipeline"]["hmplaycropper"]["plot-play-tracking"].as<int>() == 1 &&
          mapping_plot_or.config()["pipeline"]["hmplaycropper"]["plot-player-tracking"].as<int>() == 0,
      "Moving-box plotting must enable its native producer and consumer without enabling player tracking");

  fs::create_directories(games / "mapping-plot-native-precedence");
  hm::Configurator mapping_plot_native_precedence(
      "mapping-plot-native-precedence", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_plot_native_precedence_loaded = mapping_plot_native_precedence.configure().ok() &&
      mapping_plot_native_precedence.underlay_config("pipeline", mapping_structure_path.string()) &&
      mapping_plot_native_precedence.apply_config_item("plot.debug_play_tracker", "true").ok() &&
      mapping_plot_native_precedence.apply_config_item("pipeline.ds-playtracker.draw", "0").ok() &&
      mapping_plot_native_precedence.apply_config_item("pipeline.hmplaycropper.plot-play-tracking", "0").ok();
  const absl::Status mapping_plot_native_precedence_status = mapping_plot_native_precedence_loaded
      ? mapping_plot_native_precedence.apply_supported_baseline_mappings()
      : absl::InternalError("plot native-precedence fixture did not load");
  ok &= expect(
      mapping_plot_native_precedence_status.ok() &&
          mapping_plot_native_precedence.config()["pipeline"]["ds-playtracker"]["draw"].as<int>() == 0 &&
          mapping_plot_native_precedence.config()["pipeline"]["hmplaycropper"]["plot-play-tracking"].as<int>() == 0 &&
          mapping_plot_native_precedence.config()["pipeline"]["hmplaycropper"]["plot-player-tracking"].as<int>() == 1,
      "Direct native plot values must win same-rank ties while other derived debug properties remain enabled");

  const fs::path disabled_stitching_game_dir = games / "disabled-stitching";
  fs::create_directories(disabled_stitching_game_dir);
  YAML::Node disabled_stitching_game(YAML::NodeType::Map);
  disabled_stitching_game["stitching"]["enabled"] = false;
  disabled_stitching_game["game"]["videos"]["left"].push_back("missing-left.mp4");
  disabled_stitching_game["game"]["videos"]["right"].push_back("missing-right.mp4");
  std::ofstream(disabled_stitching_game_dir / "config.yaml") << YAML::Dump(disabled_stitching_game) << '\n';
  std::ofstream(disabled_stitching_game_dir / "seam_file.png") << "must remain untouched\n";
  const fs::path disabled_stitching_pipeline_path = root / "disabled-stitching-pipeline.yaml";
  std::ofstream(disabled_stitching_pipeline_path) << "application:\n  complete-configuration: 1\n"
                                                  << "hmstitcher: {}\n"
                                                  << "hmplaycropper: {}\n"
                                                  << "streammux: {}\n"
                                                  << "source0:\n"
                                                  << "  enable: 1\n"
                                                  << "  type: 2\n"
                                                  << "  uri: file:///missing-runtime-sized-input.mp4\n"
                                                  << "  source-id: 0\n";
  hm::Configurator disabled_stitching(
      "disabled-stitching", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool disabled_stitching_loaded = disabled_stitching.configure().ok() &&
      disabled_stitching.underlay_config("pipeline", disabled_stitching_pipeline_path.string());
  const absl::Status disabled_stitching_status = disabled_stitching_loaded
      ? disabled_stitching.complete_configuration(
            /*force=*/true,
            /*clean_stitching_artifacts=*/false,
            /*clean_stitching_from_control_points=*/false,
            /*clean_expected_invalidation_id=*/{},
            /*show_render_sink=*/false,
            /*show_render_scale=*/-1.0,
            disabled_stitching_pipeline_path.parent_path())
      : absl::InternalError("disabled stitching fixture did not load");
  ok &= expect(
      disabled_stitching_status.ok() &&
          disabled_stitching.config()["pipeline"]["hmstitcher"]["enable"].as<int>() == 0 &&
          !disabled_stitching.config()["pipeline"]["hmplaycropper"]["output-width"].IsDefined() &&
          !disabled_stitching.config()["pipeline"]["hmplaycropper"]["output-height"].IsDefined() &&
          fs::is_regular_file(disabled_stitching_game_dir / "seam_file.png"),
      "stitching.enabled=false must skip runtime discovery and preserve negotiated source dimensions");

  hm::Configurator disabled_stitching_clean(
      "disabled-stitching", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool disabled_stitching_clean_loaded = disabled_stitching_clean.configure().ok() &&
      disabled_stitching_clean.underlay_config("pipeline", disabled_stitching_pipeline_path.string());
  const absl::Status disabled_stitching_clean_status = disabled_stitching_clean_loaded
      ? disabled_stitching_clean.complete_configuration(
            /*force=*/true,
            /*clean_stitching_artifacts=*/true,
            /*clean_stitching_from_control_points=*/false,
            /*clean_expected_invalidation_id=*/{},
            /*show_render_sink=*/false,
            /*show_render_scale=*/-1.0,
            disabled_stitching_pipeline_path.parent_path())
      : absl::InternalError("disabled stitching clean fixture did not load");
  ok &= expect(
      !disabled_stitching_clean_status.ok() && fs::is_regular_file(disabled_stitching_game_dir / "seam_file.png"),
      "stitching.enabled=false must not own direct clean-only artifact cleanup");

  const fs::path canonical_clean_game_dir = games / "canonical-clean-stitching";
  fs::create_directories(canonical_clean_game_dir);
  std::ofstream(canonical_clean_game_dir / "seam_file.png") << "must be cleaned\n";
  const fs::path canonical_clean_pipeline_path = root / "canonical-clean-stitching-pipeline.yaml";
  std::ofstream(canonical_clean_pipeline_path) << "application:\n  complete-configuration: 1\n"
                                               << "hmstitcher: {}\n";
  hm::Configurator canonical_clean_stitching(
      "canonical-clean-stitching", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool canonical_clean_stitching_loaded = canonical_clean_stitching.configure().ok() &&
      canonical_clean_stitching.underlay_config("pipeline", canonical_clean_pipeline_path.string());
  const absl::Status canonical_clean_stitching_status = canonical_clean_stitching_loaded
      ? canonical_clean_stitching.complete_configuration(
            /*force=*/true,
            /*clean_stitching_artifacts=*/true,
            /*clean_stitching_from_control_points=*/false,
            /*clean_expected_invalidation_id=*/{},
            /*show_render_sink=*/false,
            /*show_render_scale=*/-1.0,
            canonical_clean_pipeline_path.parent_path())
      : absl::InternalError("canonical clean stitching fixture did not load");
  ok &= expect(
      canonical_clean_stitching_status.code() == absl::StatusCode::kCancelled &&
          !fs::exists(canonical_clean_game_dir / "seam_file.png"),
      "canonical stitching.enabled=true must own direct clean-only artifact cleanup");

  const fs::path zero_sample_span_game_dir = games / "zero-sample-span";
  fs::create_directories(zero_sample_span_game_dir);
  YAML::Node zero_sample_span_game(YAML::NodeType::Map);
  zero_sample_span_game["stitching"]["stitch_frame_time"] = "00:00:00";
  std::ofstream(zero_sample_span_game_dir / "config.yaml") << YAML::Dump(zero_sample_span_game) << '\n';
  const fs::path zero_sample_span_pipeline_path = root / "zero-sample-span-pipeline.yaml";
  std::ofstream(zero_sample_span_pipeline_path) << "application:\n"
                                                << "  complete-configuration: 1\n"
                                                << "hmstitcher:\n"
                                                << "  enable: 1\n"
                                                << "  one-pass-mode: 1\n"
                                                << "  calibration-sample-span-ns: 120000000000\n"
                                                << "  calibration_sample_span_ns: 120000000000\n"
                                                << "  private-properties:\n"
                                                << "    calibration-sample-span-ns: 120000000000\n"
                                                << "    calibration_sample_span_ns: 120000000000\n"
                                                << "hmplaycropper: {}\n"
                                                << "streammux: {}\n"
                                                << "source0:\n"
                                                << "  enable: 1\n"
                                                << "  type: 5\n"
                                                << "  source-id: 0\n"
                                                << "  camera-width: 1920\n"
                                                << "  camera-height: 1080\n"
                                                << "source1:\n"
                                                << "  enable: 1\n"
                                                << "  type: 5\n"
                                                << "  source-id: 1\n"
                                                << "  camera-width: 1920\n"
                                                << "  camera-height: 1080\n";
  hm::Configurator zero_sample_span("zero-sample-span", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool zero_sample_span_loaded = zero_sample_span.configure().ok() &&
      zero_sample_span.underlay_config("pipeline", zero_sample_span_pipeline_path.string());
  const absl::Status zero_sample_span_status = zero_sample_span_loaded
      ? zero_sample_span.complete_configuration(
            /*force=*/false,
            /*clean_stitching_artifacts=*/false,
            /*clean_stitching_from_control_points=*/false,
            /*clean_expected_invalidation_id=*/{},
            /*show_render_sink=*/false,
            /*show_render_scale=*/-1.0,
            zero_sample_span_pipeline_path.parent_path())
      : absl::InternalError("zero sample-span fixture did not load");
  const YAML::Node zero_sample_stitcher = zero_sample_span.config()["pipeline"]["hmstitcher"];
  ok &= expect(
      zero_sample_span_status.ok() && !zero_sample_stitcher["calibration-sample-span-ns"].IsDefined() &&
          !zero_sample_stitcher["calibration_sample_span_ns"].IsDefined() &&
          !zero_sample_stitcher["private-properties"]["calibration-sample-span-ns"].IsDefined() &&
          !zero_sample_stitcher["private-properties"]["calibration_sample_span_ns"].IsDefined(),
      "Zero stitch-frame calibration must remove all sample-span aliases so startup captures the first pairs");

  const fs::path clear_game_dir = games / "mapping-explicit-clear";
  fs::create_directories(clear_game_dir);
  YAML::Node explicit_clears(YAML::NodeType::Map);
  explicit_clears["video_out"]["output_video_path"] = YAML::Node(YAML::NodeType::Null);
  explicit_clears["video_out"]["output_width"] = "auto";
  explicit_clears["video_out"]["output_height"] = YAML::Node(YAML::NodeType::Null);
  explicit_clears["rink"]["scoreboard"]["perspective_polygon"] = YAML::Node(YAML::NodeType::Null);
  std::ofstream(clear_game_dir / "config.yaml") << YAML::Dump(explicit_clears) << '\n';
  hm::Configurator mapping_explicit_clear(
      "mapping-explicit-clear", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const bool mapping_explicit_clear_loaded = mapping_explicit_clear.configure().ok() &&
      mapping_explicit_clear.underlay_config("pipeline", structural_custom_path.string());
  const absl::Status mapping_explicit_clear_status = mapping_explicit_clear_loaded
      ? mapping_explicit_clear.apply_supported_baseline_mappings()
      : absl::InternalError("mapping explicit clear fixture did not load");
  const YAML::Node mapped_clear = mapping_explicit_clear.config()["pipeline"];
  ok &= expect(
      mapping_explicit_clear_status.ok() && !mapped_clear["sink0"]["output-file"].IsDefined() &&
          !mapped_clear["sink0"]["width"].IsDefined() && !mapped_clear["sink0"]["height"].IsDefined() &&
          !mapped_clear["hmplaycropper"]["scoreboard-perspective-polygon"].IsDefined(),
      "Explicit canonical null/auto values must clear lower-ranked native output and scoreboard fields");

  const fs::path no_home_games = root / "no-home-games";
  const fs::path no_home_output = root / "no-home-output";
  fs::create_directories(no_home_games / "explicit-roots");
  std::ofstream(no_home_games / "explicit-roots" / "config.yaml") << "pipeline:\n  private-without-home: yes\n";
  ::unsetenv("HOME");
  ::setenv("HM_GAME_DIR", no_home_games.c_str(), 1);
  ::setenv("HM_OUTPUT_WORK_DIR", no_home_output.c_str(), 1);
  const auto no_home_overlay = hm::user_config::load_or_create();
  hm::Configurator no_home_configurator("explicit-roots", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const auto no_home_config = no_home_configurator.load_config();
  const auto no_home_game_root = no_home_overlay.ok() ? hm::user_config::game_root(*no_home_overlay)
                                                      : absl::StatusOr<fs::path>(no_home_overlay.status());
  const auto no_home_output_root = no_home_overlay.ok() ? hm::user_config::output_root(*no_home_overlay)
                                                        : absl::StatusOr<fs::path>(no_home_overlay.status());
  ok &= expect(
      no_home_overlay.ok() && no_home_overlay->IsMap() && no_home_overlay->size() == 0 && no_home_config.ok() &&
          (*no_home_config)["pipeline"]["baseline-only"].as<std::string>() == "yes" &&
          (*no_home_config)["pipeline"]["private-without-home"].as<std::string>() == "yes" && no_home_game_root.ok() &&
          *no_home_game_root == no_home_games && no_home_output_root.ok() && *no_home_output_root == no_home_output,
      "Explicit game/output roots must keep configuration usable when HOME is unset");
  ::setenv("HOME", test_home.c_str(), 1);
  ::unsetenv("HM_OUTPUT_WORK_DIR");
  ::setenv("HM_GAME_DIR", games.c_str(), 1);

  const fs::path layered_game = games / "layered";
  fs::create_directories(layered_game);
  std::ofstream(layered_game / "config.yaml") << "pipeline:\n  layered-value: private\n  private-only: yes\n";
  hm::Configurator layered("layered", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const auto layered_config = layered.load_config();
  ok &= expect(
      layered_config.ok() && (*layered_config)["pipeline"]["layered-value"].as<std::string>() == "private" &&
          (*layered_config)["pipeline"]["baseline-only"].as<std::string>() == "yes" &&
          (*layered_config)["pipeline"]["user-only"].as<std::string>() == "yes" &&
          (*layered_config)["pipeline"]["private-only"].as<std::string>() == "yes" &&
          (*layered_config)["stitching"]["stitch_frame_time"].as<std::string>() == "00:00:08" &&
          !hm::get_node(layered.game_private_config(), "stitching.stitch_frame_time").has_value() &&
          !fs::exists(layered_game / ".hstream-stitch.lock"),
      "Config precedence must be baseline, then user overlay, then game-private YAML without artifact recovery for "
      "an ordinary config");
  ::unsetenv("HM_GAME_DIR");
  ok &= expect(
      hm::Configurator::get_game_dir("layered") == games / "layered",
      "The user overlay game-root must replace the HOME/Videos default when no environment override is present");

  const fs::path snapshot_root_a = root / "snapshot-root-a";
  const fs::path snapshot_root_b = root / "snapshot-root-b";
  fs::create_directories(snapshot_root_a / "snapshot-game");
  fs::create_directories(snapshot_root_b / "snapshot-game");
  std::ofstream(snapshot_root_a / "snapshot-game" / "config.yaml") << "pipeline:\n  snapshot-origin: a\n";
  user_overlay[hm::user_config::kPathsKey][hm::user_config::kGameRootKey] = snapshot_root_a.string();
  std::ofstream(user_config_path) << YAML::Dump(user_overlay) << '\n';
  hm::Configurator snapshot_configurator("snapshot-game", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const auto snapshot_loaded = snapshot_configurator.load_config();
  user_overlay[hm::user_config::kPathsKey][hm::user_config::kGameRootKey] = snapshot_root_b.string();
  std::ofstream(user_config_path) << YAML::Dump(user_overlay) << '\n';
  YAML::Node snapshot_save(YAML::NodeType::Map);
  snapshot_save["pipeline"]["snapshot-save"] = true;
  const absl::Status snapshot_save_status = snapshot_configurator.save_private_config(snapshot_save);
  auto snapshot_a_config = hm::stitching::load_game_config_file(snapshot_root_a / "snapshot-game" / "config.yaml");
  auto snapshot_b_config = hm::stitching::load_game_config_file(snapshot_root_b / "snapshot-game" / "config.yaml");
  ok &= expect(
      snapshot_loaded.ok() && (*snapshot_loaded)["pipeline"]["snapshot-origin"].as<std::string>() == "a" &&
          snapshot_save_status.ok() && snapshot_a_config.ok() && snapshot_a_config->has_value() &&
          (**snapshot_a_config)["pipeline"]["snapshot-save"].as<bool>() && snapshot_b_config.ok() &&
          !snapshot_b_config->has_value(),
      "Each Configurator must use one game-root snapshot for private-config load, lock, merge, and save");

  user_overlay[hm::user_config::kPathsKey][hm::user_config::kGameRootKey] = games.string();
  std::ofstream(user_config_path) << YAML::Dump(user_overlay) << '\n';
  ::setenv("HM_GAME_DIR", games.c_str(), 1);

  const fs::path canvas_cache_game = games / "canvas-cache-save";
  fs::create_directories(canvas_cache_game);
  std::ofstream(canvas_cache_game / "config.yaml") << "generation: old\n";
  std::ofstream(canvas_cache_game / "rink_mask_0.png") << "old-mask\n";
  std::ofstream(canvas_cache_game / "s.png") << "old-stitched-snapshot\n";
  hm::Configurator canvas_cache_configurator(
      "canvas-cache-save", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const auto canvas_cache_loaded = canvas_cache_configurator.load_config();
  YAML::Node canvas_cache_desired(YAML::NodeType::Map);
  canvas_cache_desired["generation"] = "new";
  const absl::Status canvas_cache_saved = canvas_cache_loaded.ok()
      ? canvas_cache_configurator.save_private_config(
            canvas_cache_desired, /*expected_invalidation_id=*/{}, /*remove_canvas_artifacts=*/true)
      : canvas_cache_loaded.status();
  ok &= expect(
      canvas_cache_saved.ok() && !fs::exists(canvas_cache_game / "rink_mask_0.png") &&
          !fs::exists(canvas_cache_game / "s.png") &&
          YAML::LoadFile((canvas_cache_game / "config.yaml").string())["generation"].as<std::string>() == "new",
      "Configurator canvas invalidation must transactionally remove rink masks and the stitched snapshot");

  const fs::path tracker_base_path = root / "custom-playtracker.yaml";
  std::ofstream(tracker_base_path) << R"(play-tracker:
  preserve-custom-root: true
  no-wide-start: false
  live-boxes:
    - name: current_roi
    - name: current_roi_aspect
      sticky-translation-gaussian-mult: 9.5
)";
  const fs::path incomplete_pipeline = root / "incomplete-pipeline.yaml";
  std::ofstream(incomplete_pipeline) << "application:\n"
                                     << "  complete-configuration: 0\n"
                                     << "ds-playtracker:\n"
                                     << "  enable: 1\n"
                                     << "  config-file: " << tracker_base_path.string() << "\n";
  const fs::path incomplete_game_dir = games / "materialize-incomplete";
  fs::create_directories(incomplete_game_dir);
  const fs::path runtime_root = root / "runtime";
  fs::create_directories(runtime_root);
  ::setenv("XDG_RUNTIME_DIR", runtime_root.c_str(), 1);
  if (bundled_baseline.ok()) {
    const fs::path structural_tracker_dir = root / "structural-tracker-config";
    fs::create_directories(structural_tracker_dir);
    std::ofstream(structural_tracker_dir / "play_tracker_config.yaml") << R"(play-tracker:
  structural-owner: true
  live-boxes:
    - name: current_roi
    - name: current_roi_aspect
)";
    const fs::path collision_game_dir = games / "tracker-structural-path-collision";
    fs::create_directories(collision_game_dir);
    std::ofstream(collision_game_dir / "play_tracker_config.yaml") << "stale-game-owner: true\n";
    const fs::path structural_tracker_pipeline = structural_tracker_dir / "pipeline.yaml";
    std::ofstream(structural_tracker_pipeline) << "application:\n  complete-configuration: 0\n"
                                               << "ds-playtracker:\n"
                                               << "  enable: 1\n"
                                               << "  config-file: play_tracker_config.yaml\n";
    hm::Configurator structural_tracker(
        "tracker-structural-path-collision", bundled_baseline->root.string(), hm::Configurator::kUseConfigFileGpu);
    const bool structural_tracker_loaded = structural_tracker.configure().ok() &&
        structural_tracker.underlay_config("pipeline", structural_tracker_pipeline.string());
    const absl::Status structural_tracker_status = structural_tracker_loaded
        ? structural_tracker.complete_configuration(
              /*force=*/false,
              /*clean_stitching_artifacts=*/false,
              /*clean_stitching_from_control_points=*/false,
              /*clean_expected_invalidation_id=*/{},
              /*show_render_sink=*/false,
              /*show_render_scale=*/-1.0,
              structural_tracker_pipeline.parent_path())
        : absl::InternalError("structural tracker path fixture did not load");
    const fs::path structural_tracker_effective_path = structural_tracker_status.ok()
        ? fs::path(structural_tracker.config()["pipeline"]["ds-playtracker"]["config-file"].as<std::string>())
        : fs::path();
    const YAML::Node structural_tracker_effective =
        structural_tracker_status.ok() && fs::is_regular_file(structural_tracker_effective_path)
        ? YAML::LoadFile(structural_tracker_effective_path.string())
        : YAML::Node();
    ok &= expect(
        structural_tracker_status.ok() && structural_tracker_effective["play-tracker"]["structural-owner"].as<bool>(),
        "A structural relative tracker config must resolve beside its pipeline config before the game directory");

    const fs::path disabled_tracker_game_dir = games / "tracker-disabled";
    fs::create_directories(disabled_tracker_game_dir);
    const fs::path disabled_tracker_pipeline = root / "disabled-tracker-pipeline.yaml";
    std::ofstream(disabled_tracker_pipeline) << "application:\n  complete-configuration: 0\n"
                                             << "ds-playtracker:\n"
                                             << "  enable: 0\n";
    hm::Configurator disabled_tracker(
        "tracker-disabled", bundled_baseline->root.string(), hm::Configurator::kUseConfigFileGpu);
    const bool disabled_tracker_loaded = disabled_tracker.configure().ok() &&
        disabled_tracker.underlay_config("pipeline", disabled_tracker_pipeline.string());
    const absl::Status disabled_tracker_status = disabled_tracker_loaded
        ? disabled_tracker.complete_configuration(
              /*force=*/false,
              /*clean_stitching_artifacts=*/false,
              /*clean_stitching_from_control_points=*/false,
              /*clean_expected_invalidation_id=*/{},
              /*show_render_sink=*/false,
              /*show_render_scale=*/-1.0,
              disabled_tracker_pipeline.parent_path())
        : absl::InternalError("disabled tracker fixture did not load");
    ok &= expect(
        disabled_tracker_status.ok() &&
            !disabled_tracker.config()["pipeline"]["ds-playtracker"]["config-file"].IsDefined() &&
            !fs::exists(disabled_tracker_game_dir / ".hstream-runtime"),
        "A disabled tracker must not require or materialize a runtime sidecar");

    hm::Configurator incomplete_configurator(
        "materialize-incomplete", bundled_baseline->root.string(), hm::Configurator::kUseConfigFileGpu);
    const bool incomplete_loaded = incomplete_configurator.configure().ok() &&
        incomplete_configurator.underlay_config("pipeline", incomplete_pipeline.string());
    const absl::Status incomplete_status = incomplete_loaded
        ? incomplete_configurator.complete_configuration(
              /*force=*/false,
              /*clean_stitching_artifacts=*/false,
              /*clean_stitching_from_control_points=*/false,
              /*clean_expected_invalidation_id=*/{},
              /*show_render_sink=*/false,
              /*show_render_scale=*/-1.0,
              incomplete_pipeline.parent_path())
        : absl::InternalError("incomplete materialization fixture did not load");
    const fs::path incomplete_effective_path = incomplete_status.ok()
        ? fs::path(incomplete_configurator.config()["pipeline"]["ds-playtracker"]["config-file"].as<std::string>())
        : fs::path();
    YAML::Node incomplete_effective = incomplete_status.ok() && fs::is_regular_file(incomplete_effective_path)
        ? YAML::LoadFile(incomplete_effective_path.string())
        : YAML::Node();
    ok &= expect(
        incomplete_status.ok() && incomplete_effective_path.parent_path() == incomplete_game_dir / ".hstream-runtime" &&
            incomplete_effective["play-tracker"]["preserve-custom-root"].as<bool>() &&
            !incomplete_effective["play-tracker"]["no-wide-start"].as<bool>() &&
            incomplete_effective["play-tracker"]["live-boxes"][1]["sticky-translation-gaussian-mult"].as<double>() ==
                9.5 &&
            incomplete_effective["play-tracker"]["live-boxes"][0]["time-to-dest-speed-limit-frames"].as<int>() == 20,
        "complete-configuration=false must still underlay every missing tracker field from bundled baseline");

    hm::Configurator raw_configurator("", bundled_baseline->root.string(), hm::Configurator::kUseConfigFileGpu);
    const bool raw_loaded =
        raw_configurator.configure().ok() && raw_configurator.underlay_config("pipeline", incomplete_pipeline.string());
    const absl::Status raw_status = raw_loaded ? raw_configurator.complete_configuration(
                                                     /*force=*/false,
                                                     /*clean_stitching_artifacts=*/false,
                                                     /*clean_stitching_from_control_points=*/false,
                                                     /*clean_expected_invalidation_id=*/{},
                                                     /*show_render_sink=*/false,
                                                     /*show_render_scale=*/-1.0,
                                                     incomplete_pipeline.parent_path())
                                               : absl::InternalError("raw materialization fixture did not load");
    const fs::path raw_effective_path = raw_status.ok()
        ? fs::path(raw_configurator.config()["pipeline"]["ds-playtracker"]["config-file"].as<std::string>())
        : fs::path();
    ok &= expect(
        raw_status.ok() && raw_effective_path.parent_path() == runtime_root / "hstream" &&
            fs::is_regular_file(raw_effective_path) && raw_effective_path.parent_path() != fs::current_path(),
        "No-game raw launches must materialize baseline-backed tracker config in a per-user runtime directory");

    user_overlay["rink"]["camera"]["sticky_translation_gaussian_mult"] = 6.5;
    std::ofstream(user_config_path) << YAML::Dump(user_overlay) << '\n';
    auto materialized_sticky_value = [&](const std::string& game_id,
                                         std::optional<double> game_canonical,
                                         std::optional<double> cli_canonical) -> absl::StatusOr<double> {
      const fs::path provenance_game_dir = games / game_id;
      fs::create_directories(provenance_game_dir);
      YAML::Node game_config(YAML::NodeType::Map);
      game_config["pipeline"]["ds-playtracker"]["config-file"] = tracker_base_path.string();
      if (game_canonical.has_value())
        game_config["rink"]["camera"]["sticky_translation_gaussian_mult"] = *game_canonical;
      std::ofstream(provenance_game_dir / "config.yaml") << YAML::Dump(game_config) << '\n';
      hm::Configurator provenance(game_id, bundled_baseline->root.string(), hm::Configurator::kUseConfigFileGpu);
      if (!provenance.configure().ok() || !provenance.underlay_config("pipeline", incomplete_pipeline.string()))
        return absl::InternalError("Could not load tracker provenance fixture");
      if (cli_canonical.has_value()) {
        HM_RETURN_IF_ERROR(provenance.apply_config_item(
            "rink.camera.sticky_translation_gaussian_mult", std::to_string(*cli_canonical)));
      }
      HM_RETURN_IF_ERROR(provenance.complete_configuration(
          /*force=*/false,
          /*clean_stitching_artifacts=*/false,
          /*clean_stitching_from_control_points=*/false,
          /*clean_expected_invalidation_id=*/{},
          /*show_render_sink=*/false,
          /*show_render_scale=*/-1.0,
          incomplete_pipeline.parent_path()));
      const fs::path generated = provenance.config()["pipeline"]["ds-playtracker"]["config-file"].as<std::string>();
      try {
        return YAML::LoadFile(generated.string())["play-tracker"]["live-boxes"][1]["sticky-translation-gaussian-mult"]
            .as<double>();
      } catch (const std::exception& error) {
        return absl::InvalidArgumentError(std::string("Could not read materialized tracker fixture: ") + error.what());
      }
    };
    const auto user_canonical_game_native =
        materialized_sticky_value("tracker-user-vs-game-native", std::nullopt, std::nullopt);
    const auto game_canonical_game_native = materialized_sticky_value("tracker-game-tie", 7.5, std::nullopt);
    const auto cli_canonical_game_native = materialized_sticky_value("tracker-cli-vs-game-native", 7.5, 8.5);
    ok &= expect(
        user_canonical_game_native.ok() && *user_canonical_game_native == 9.5 && game_canonical_game_native.ok() &&
            *game_canonical_game_native == 9.5 && cli_canonical_game_native.ok() && *cli_canonical_game_native == 8.5,
        "Tracker materialization must honor layer rank and prefer direct native values on same-rank ties");
    user_overlay["rink"]["camera"].remove("sticky_translation_gaussian_mult");
    std::ofstream(user_config_path) << YAML::Dump(user_overlay) << '\n';

    fs::last_write_time(incomplete_effective_path, fs::file_time_type::clock::now() - std::chrono::hours(48));
    const fs::path game_runtime_dir = incomplete_effective_path.parent_path();
    for (int index = 0; index < 12; ++index) {
      std::ostringstream stale_name;
      stale_name << "play_tracker_config-" << std::hex << (0x1000 + index) << ".yaml";
      const fs::path stale = game_runtime_dir / stale_name.str();
      std::ofstream(stale) << "stale: " << index << '\n';
      fs::last_write_time(
          stale, fs::file_time_type::clock::now() - std::chrono::hours(47) + std::chrono::minutes(index));
    }
    hm::Configurator retention_configurator(
        "materialize-incomplete", bundled_baseline->root.string(), hm::Configurator::kUseConfigFileGpu);
    const bool retention_loaded = retention_configurator.configure().ok() &&
        retention_configurator.underlay_config("pipeline", incomplete_pipeline.string()) &&
        retention_configurator.apply_config_item("rink.camera.sticky_translation_gaussian_mult", "6.0").ok();
    const absl::Status retention_status = retention_loaded
        ? retention_configurator.complete_configuration(
              /*force=*/false,
              /*clean_stitching_artifacts=*/false,
              /*clean_stitching_from_control_points=*/false,
              /*clean_expected_invalidation_id=*/{},
              /*show_render_sink=*/false,
              /*show_render_scale=*/-1.0,
              incomplete_pipeline.parent_path())
        : absl::InternalError("retention materialization fixture did not load");
    size_t retained_yaml_count = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(game_runtime_dir)) {
      if (entry.is_regular_file() && entry.path().filename().string().rfind("play_tracker_config-", 0) == 0 &&
          entry.path().extension() == ".yaml") {
        ++retained_yaml_count;
      }
    }
    ok &= expect(
        retention_status.ok() && fs::is_regular_file(incomplete_effective_path) && retained_yaml_count <= 9,
        "Tracker runtime retention must prune stale generations while preserving a file locked by an active run");
  } else {
    ok &= expect(false, bundled_baseline.status().ToString().c_str());
  }

  hm::Configurator configurator("first-save", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const auto loaded = configurator.load_config();
  ok &= expect(loaded.ok(), "Configurator must load when the private config is initially absent");

  {
    auto lock = hm::stitching::GameConfigTransactionLock::Acquire(game_dir);
    ok &= expect(lock.ok(), "concurrent config creator must acquire the transaction lock");
    if (lock.ok()) {
      YAML::Node concurrent(YAML::NodeType::Map);
      concurrent["hstream_ui"]["keep"] = true;
      ok &= expect(
          hm::stitching::publish_game_config(game_dir, YAML::Dump(concurrent) + "\n").ok(),
          "concurrent config creation must publish");
    }
  }

  YAML::Node desired(YAML::NodeType::Map);
  desired["pipeline"]["generated"] = true;
  ok &= expect(
      configurator.save_private_config(desired).ok(), "Configurator first save after concurrent creation must publish");

  auto final_config = hm::stitching::load_game_config_file(game_dir / "config.yaml");
  ok &= expect(
      final_config.ok() && final_config->has_value() && (**final_config)["pipeline"]["generated"].as<bool>() &&
          (**final_config)["hstream_ui"]["keep"].as<bool>(),
      "Configurator first save must retain keys created after its absent baseline");

  const fs::path stitch_override_dir = games / "stitch-override";
  fs::create_directories(stitch_override_dir);
  YAML::Node stitch_override_config(YAML::NodeType::Map);
  stitch_override_config["stitching"]["stitch_frame_time"] = "00:00:07";
  stitch_override_config["hstream_ui"]["stitching_calibration"]["status"] = "pending";
  stitch_override_config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "stitch-override-a";
  ok &= expect(
      hm::stitching::publish_game_config(stitch_override_dir, YAML::Dump(stitch_override_config) + "\n").ok(),
      "stitch-frame override fixture must publish");
  hm::Configurator stitch_override("stitch-override", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const auto stitch_override_loaded = stitch_override.load_config();
  const absl::Status zero_override_status = stitch_override.persist_stitch_frame_time_override({});
  auto after_zero_override = hm::stitching::load_game_config_file(stitch_override_dir / "config.yaml");
  hm::Configurator stitch_override_reloaded(
      "stitch-override", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const auto stitch_override_reloaded_config = stitch_override_reloaded.load_config();
  const absl::Status fractional_override_status = stitch_override.persist_stitch_frame_time_override("00:00:00.500");
  auto after_fractional_override = hm::stitching::load_game_config_file(stitch_override_dir / "config.yaml");
  ok &= expect(
      stitch_override_loaded.ok() && zero_override_status.ok() && after_zero_override.ok() &&
          after_zero_override->has_value() &&
          (**after_zero_override)["stitching"]["stitch_frame_time"].as<std::string>() == "00:00:00" &&
          stitch_override_reloaded_config.ok() &&
          (*stitch_override_reloaded_config)["stitching"]["stitch_frame_time"].as<std::string>() == "00:00:00" &&
          hm::get_node(stitch_override_reloaded.game_private_config(), "stitching.stitch_frame_time").has_value() &&
          (**after_zero_override)["hstream_ui"]["stitching_calibration"]["invalidation_id"].as<std::string>() ==
              "stitch-override-a" &&
          fractional_override_status.ok() && after_fractional_override.ok() && after_fractional_override->has_value() &&
          (**after_fractional_override)["stitching"]["stitch_frame_time"].as<std::string>() == "00:00:00.500",
      "Runtime stitch-frame overrides must persist zero against a nonzero lower layer, persist nonzero, and preserve "
      "the "
      "pending generation owner");

  const fs::path backend_choices_baseline_root = root / "backend-choices-baseline";
  fs::create_directories(backend_choices_baseline_root);
  YAML::Node backend_choices_baseline =
      bundled_baseline.ok() ? YAML::Clone(bundled_baseline->values) : YAML::Node(YAML::NodeType::Map);
  backend_choices_baseline["stitching"]["control_point_matcher"] = "native-aliked-lightglue";
  backend_choices_baseline["stitching"]["mapping_backend"] = "magsac";
  backend_choices_baseline["stitching"]["run_autooptimizer"] = true;
  backend_choices_baseline["stitching"]["projection"] = "planar";
  std::ofstream(backend_choices_baseline_root / "baseline.yaml") << YAML::Dump(backend_choices_baseline) << '\n';
  const fs::path backend_choices_dir = games / "backend-choices";
  fs::create_directories(backend_choices_dir);
  hm::Configurator backend_choices(
      "backend-choices", backend_choices_baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status backend_choices_configured = backend_choices.configure();
  const absl::Status backend_choices_persisted = backend_choices.persist_effective_stitching_backend_choices();
  auto backend_choices_private = hm::stitching::load_game_config_file(backend_choices_dir / "config.yaml");
  ok &= expect(
      backend_choices_configured.ok() && backend_choices_persisted.ok() && backend_choices_private.ok() &&
          backend_choices_private->has_value() &&
          (**backend_choices_private)["stitching"]["control_point_matcher"].as<std::string>() ==
              "superpoint-lightglue" &&
          (**backend_choices_private)["stitching"]["mapping_backend"].as<std::string>() == "opencv-magsac" &&
          (**backend_choices_private)["stitching"]["projection"].as<std::string>() == "rectilinear" &&
          (**backend_choices_private)["stitching"]["run_autooptimizer"].as<bool>(),
      "Effective stitching algorithm/projection choices from lower config layers must be materialized into "
      "game-private config with canonical spellings");
  const fs::path backend_omitted_baseline_root = root / "backend-omitted-baseline";
  fs::create_directories(backend_omitted_baseline_root);
  YAML::Node backend_omitted_baseline =
      bundled_baseline.ok() ? YAML::Clone(bundled_baseline->values) : YAML::Node(YAML::NodeType::Map);
  backend_omitted_baseline["stitching"].remove("mapping_backend");
  backend_omitted_baseline["stitching"].remove("projection");
  backend_omitted_baseline["stitching"].remove("run_autooptimizer");
  std::ofstream(backend_omitted_baseline_root / "baseline.yaml") << YAML::Dump(backend_omitted_baseline) << '\n';
  const fs::path backend_omitted_dir = games / "backend-omitted";
  fs::create_directories(backend_omitted_dir);
  hm::Configurator backend_omitted(
      "backend-omitted", backend_omitted_baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status backend_omitted_configured = backend_omitted.configure();
  const absl::Status backend_omitted_persisted = backend_omitted.persist_effective_stitching_backend_choices();
  auto backend_omitted_private = hm::stitching::load_game_config_file(backend_omitted_dir / "config.yaml");
  ok &= expect(
      backend_omitted_configured.ok() && backend_omitted_persisted.ok() && backend_omitted_private.ok() &&
          backend_omitted_private->has_value() &&
          (**backend_omitted_private)["stitching"]["mapping_backend"].as<std::string>() == "opencv-magsac" &&
          (**backend_omitted_private)["stitching"]["projection"].as<std::string>() == "rectilinear" &&
          !(**backend_omitted_private)["stitching"]["run_autooptimizer"].as<bool>(),
      "Missing algorithm keys in an older baseline must materialize valid rectilinear "
      "MAGSAC-without-autooptimizer defaults");
  backend_choices_baseline["stitching"]["mapping_backend"] = "affine-ransac";
  backend_choices_baseline["stitching"]["run_autooptimizer"] = false;
  std::ofstream(backend_choices_baseline_root / "baseline.yaml") << YAML::Dump(backend_choices_baseline) << '\n';
  hm::Configurator backend_choices_changed(
      "backend-choices", backend_choices_baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status backend_choices_changed_configured = backend_choices_changed.configure();
  const absl::Status backend_choices_changed_persisted =
      backend_choices_changed.persist_effective_stitching_backend_choices();
  auto backend_choices_changed_private = hm::stitching::load_game_config_file(backend_choices_dir / "config.yaml");
  ok &= expect(
      backend_choices_changed_configured.ok() && backend_choices_changed_persisted.ok() &&
          backend_choices_changed_private.ok() && backend_choices_changed_private->has_value() &&
          (**backend_choices_changed_private)["stitching"]["mapping_backend"].as<std::string>() ==
              "opencv-affine-ransac" &&
          (**backend_choices_changed_private)["stitching"]["projection"].as<std::string>() == "rectilinear" &&
          !(**backend_choices_changed_private)["stitching"]["run_autooptimizer"].as<bool>(),
      "Generated game-private stitching algorithm/projection choices must not mask later lower-layer config changes");

  const fs::path incompatible_projection_root = root / "incompatible-projection-baseline";
  fs::create_directories(incompatible_projection_root);
  YAML::Node incompatible_projection_baseline = YAML::Clone(backend_choices_baseline);
  incompatible_projection_baseline["stitching"]["mapping_backend"] = "opencv-magsac";
  incompatible_projection_baseline["stitching"]["projection"] = "cylindrical";
  std::ofstream(incompatible_projection_root / "baseline.yaml") << YAML::Dump(incompatible_projection_baseline) << '\n';
  fs::create_directories(games / "incompatible-projection");
  hm::Configurator incompatible_projection(
      "incompatible-projection", incompatible_projection_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status incompatible_projection_configured = incompatible_projection.configure();
  const absl::Status incompatible_projection_status = incompatible_projection_configured.ok()
      ? incompatible_projection.persist_effective_stitching_backend_choices()
      : incompatible_projection_configured;
  ok &= expect(
      absl::IsInvalidArgument(incompatible_projection_status) &&
          std::string(incompatible_projection_status.message()).find("supports only rectilinear output") !=
              std::string::npos,
      "An explicitly incompatible YAML mapping backend/projection pair must fail configuration");

  const fs::path nona_projection_root = root / "nona-projection-baseline";
  fs::create_directories(nona_projection_root);
  YAML::Node nona_projection_baseline = YAML::Clone(backend_choices_baseline);
  nona_projection_baseline["stitching"]["mapping_backend"] = "nona";
  nona_projection_baseline["stitching"]["projection"] = "cylindrical";
  nona_projection_baseline["stitching"]["run_autooptimizer"] = true;
  std::ofstream(nona_projection_root / "baseline.yaml") << YAML::Dump(nona_projection_baseline) << '\n';
  fs::create_directories(games / "nona-projection");
  hm::Configurator nona_projection(
      "nona-projection", nona_projection_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status nona_projection_configured = nona_projection.configure();
  const absl::Status nona_projection_status = nona_projection_configured.ok()
      ? nona_projection.persist_effective_stitching_backend_choices()
      : nona_projection_configured;
  ok &= expect(nona_projection_status.ok(), "Nona must accept supported non-rectilinear YAML projections");
  const fs::path backend_cli_dir = games / "backend-cli";
  fs::create_directories(backend_cli_dir);
  YAML::Node backend_cli_private(YAML::NodeType::Map);
  backend_cli_private["stitching"]["control_point_matcher"] = "superpoint-lightglue";
  backend_cli_private["stitching"]["mapping_backend"] = "opencv-affine-ransac";
  backend_cli_private["stitching"]["run_autooptimizer"] = false;
  ok &= expect(
      hm::stitching::publish_game_config(backend_cli_dir, YAML::Dump(backend_cli_private) + "\n").ok(),
      "backend CLI fixture must publish");
  hm::Configurator backend_cli("backend-cli", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status backend_cli_configured = backend_cli.configure();
  const absl::Status backend_cli_option = backend_cli.apply_config_item("stitching.mapping_backend", "opencv-magsac");
  const absl::Status backend_cli_autooptimizer = backend_cli.apply_config_item("stitching.run_autooptimizer", "true");
  const absl::Status backend_cli_persisted = backend_cli.persist_effective_stitching_backend_choices();
  auto backend_cli_generated = hm::stitching::load_game_config_file(backend_cli_dir / "config.yaml");
  hm::Configurator backend_cli_repeated("backend-cli", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status backend_cli_repeated_configured = backend_cli_repeated.configure();
  const absl::Status backend_cli_repeated_option =
      backend_cli_repeated.apply_config_item("stitching.mapping_backend", "opencv-magsac");
  const absl::Status backend_cli_repeated_autooptimizer =
      backend_cli_repeated.apply_config_item("stitching.run_autooptimizer", "true");
  const absl::Status backend_cli_repeated_persisted =
      backend_cli_repeated.persist_effective_stitching_backend_choices();
  auto backend_cli_repeated_generated = hm::stitching::load_game_config_file(backend_cli_dir / "config.yaml");
  hm::Configurator backend_cli_reloaded("backend-cli", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status backend_cli_reconfigured = backend_cli_reloaded.configure();
  const absl::Status backend_cli_restored = backend_cli_reloaded.persist_effective_stitching_backend_choices();
  auto backend_cli_final = hm::stitching::load_game_config_file(backend_cli_dir / "config.yaml");
  ok &= expect(
      backend_cli_configured.ok() && backend_cli_option.ok() && backend_cli_autooptimizer.ok() &&
          backend_cli_persisted.ok() && backend_cli_generated.ok() && backend_cli_generated->has_value() &&
          (**backend_cli_generated)["stitching"]["mapping_backend"].as<std::string>() == "opencv-magsac" &&
          (**backend_cli_generated)["stitching"]["projection"].as<std::string>() == "rectilinear" &&
          (**backend_cli_generated)["stitching"]["run_autooptimizer"].as<bool>() &&
          (**backend_cli_generated)["hstream_ui"]["generated_stitching_backend_choices"]["projection"]
                  .as<std::string>() == "rectilinear" &&
          (**backend_cli_generated)["hstream_ui"]["generated_stitching_backend_choices"]["previous_mapping_backend"]
                  .as<std::string>() == "opencv-affine-ransac" &&
          !(**backend_cli_generated)["hstream_ui"]["generated_stitching_backend_choices"]["previous_run_autooptimizer"]
               .as<bool>() &&
          backend_cli_repeated_configured.ok() && backend_cli_repeated_option.ok() &&
          backend_cli_repeated_autooptimizer.ok() && backend_cli_repeated_persisted.ok() &&
          backend_cli_repeated_generated.ok() && backend_cli_repeated_generated->has_value() &&
          (**backend_cli_repeated_generated)["hstream_ui"]["generated_stitching_backend_choices"]
                                            ["previous_mapping_backend"]
                                                .as<std::string>() == "opencv-affine-ransac" &&
          backend_cli_reconfigured.ok() && backend_cli_restored.ok() && backend_cli_final.ok() &&
          backend_cli_final->has_value() &&
          (**backend_cli_final)["stitching"]["mapping_backend"].as<std::string>() == "opencv-affine-ransac" &&
          !(**backend_cli_final)["stitching"]["run_autooptimizer"].as<bool>() &&
          !hm::get_node(**backend_cli_final, "hstream_ui.generated_stitching_backend_choices").has_value(),
      "CLI materialization must preserve and restore an existing explicit game-private stitching backend choice");

  const fs::path malformed_backend_dir = games / "malformed-backend-cli";
  fs::create_directories(malformed_backend_dir);
  YAML::Node malformed_backend_private(YAML::NodeType::Map);
  malformed_backend_private["stitching"]["control_point_matcher"] = "superpoint-lightglue";
  malformed_backend_private["stitching"]["mapping_backend"] = "opencv-affine-ransac";
  malformed_backend_private["stitching"]["run_autooptimizer"] = "invalid";
  malformed_backend_private["hstream_ui"]["generated_stitching_backend_choices"]["control_point_matcher"] =
      "superpoint-lightglue";
  malformed_backend_private["hstream_ui"]["generated_stitching_backend_choices"]["mapping_backend"] =
      "opencv-affine-ransac";
  malformed_backend_private["hstream_ui"]["generated_stitching_backend_choices"]["run_autooptimizer"] = "invalid";
  ok &= expect(
      hm::stitching::publish_game_config(malformed_backend_dir, YAML::Dump(malformed_backend_private) + "\n").ok(),
      "malformed private backend fixture must publish");
  hm::Configurator malformed_backend_cli(
      "malformed-backend-cli", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status malformed_backend_configured = malformed_backend_cli.configure();
  const absl::Status malformed_backend_mapping =
      malformed_backend_cli.apply_config_item("stitching.mapping_backend", "opencv-magsac");
  const absl::Status malformed_backend_autooptimizer =
      malformed_backend_cli.apply_config_item("stitching.run_autooptimizer", "false");
  const absl::Status malformed_backend_repaired = malformed_backend_cli.persist_effective_stitching_backend_choices();
  auto malformed_backend_after_repair = hm::stitching::load_game_config_file(malformed_backend_dir / "config.yaml");
  hm::Configurator malformed_backend_restarted(
      "malformed-backend-cli", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status malformed_backend_reconfigured = malformed_backend_restarted.configure();
  const absl::Status malformed_backend_restored =
      malformed_backend_restarted.persist_effective_stitching_backend_choices();
  auto malformed_backend_final = hm::stitching::load_game_config_file(malformed_backend_dir / "config.yaml");
  ok &= expect(
      malformed_backend_configured.ok() && malformed_backend_mapping.ok() && malformed_backend_autooptimizer.ok() &&
          malformed_backend_repaired.ok() && malformed_backend_after_repair.ok() &&
          malformed_backend_after_repair->has_value() &&
          !hm::get_node(
               **malformed_backend_after_repair,
               "hstream_ui.generated_stitching_backend_choices.previous_mapping_backend")
               .has_value() &&
          malformed_backend_reconfigured.ok() && malformed_backend_restored.ok() && malformed_backend_final.ok() &&
          malformed_backend_final->has_value() &&
          (**malformed_backend_final)["stitching"]["mapping_backend"].as<std::string>() == "opencv-magsac" &&
          !(**malformed_backend_final)["stitching"]["run_autooptimizer"].as<bool>() &&
          !hm::get_node(
               **malformed_backend_final,
               "hstream_ui.generated_stitching_backend_choices.previous_mapping_backend")
               .has_value(),
      "A valid CLI override must repair malformed private optimizer provenance across a restart without throwing");

  const fs::path legacy_backend_dir = games / "legacy-generated-backend";
  fs::create_directories(legacy_backend_dir);
  YAML::Node legacy_backend_private(YAML::NodeType::Map);
  legacy_backend_private["stitching"]["control_point_matcher"] = "superpoint-lightglue";
  legacy_backend_private["stitching"]["mapping_backend"] = "nona";
  legacy_backend_private["hstream_ui"]["generated_stitching_backend_choices"]["control_point_matcher"] =
      "superpoint-lightglue";
  legacy_backend_private["hstream_ui"]["generated_stitching_backend_choices"]["mapping_backend"] = "nona";
  ok &= expect(
      hm::stitching::publish_game_config(legacy_backend_dir, YAML::Dump(legacy_backend_private) + "\n").ok(),
      "legacy generated-backend fixture must publish");
  hm::Configurator legacy_backend_migration(
      "legacy-generated-backend", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status legacy_backend_configured = legacy_backend_migration.configure();
  const absl::Status legacy_backend_mapping =
      legacy_backend_migration.apply_config_item("stitching.mapping_backend", "opencv-affine-ransac");
  const absl::Status legacy_backend_autooptimizer =
      legacy_backend_migration.apply_config_item("stitching.run_autooptimizer", "false");
  const absl::Status legacy_backend_migrated = legacy_backend_migration.persist_effective_stitching_backend_choices();
  auto legacy_backend_after_migration = hm::stitching::load_game_config_file(legacy_backend_dir / "config.yaml");
  hm::Configurator legacy_backend_restarted(
      "legacy-generated-backend", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status legacy_backend_reconfigured = legacy_backend_restarted.configure();
  const absl::Status legacy_backend_restored = legacy_backend_restarted.persist_effective_stitching_backend_choices();
  auto legacy_backend_final = hm::stitching::load_game_config_file(legacy_backend_dir / "config.yaml");
  ok &= expect(
      legacy_backend_configured.ok() && legacy_backend_mapping.ok() && legacy_backend_autooptimizer.ok() &&
          legacy_backend_migrated.ok() && legacy_backend_after_migration.ok() &&
          legacy_backend_after_migration->has_value() &&
          !hm::get_node(
               **legacy_backend_after_migration,
               "hstream_ui.generated_stitching_backend_choices.previous_mapping_backend")
               .has_value() &&
          legacy_backend_reconfigured.ok() && legacy_backend_restored.ok() && legacy_backend_final.ok() &&
          legacy_backend_final->has_value() &&
          (**legacy_backend_final)["stitching"]["mapping_backend"].as<std::string>() == "opencv-magsac" &&
          !(**legacy_backend_final)["stitching"]["run_autooptimizer"].as<bool>() &&
          !hm::get_node(
               **legacy_backend_final, "hstream_ui.generated_stitching_backend_choices.previous_mapping_backend")
               .has_value(),
      "Legacy two-field generated backend provenance must migrate across two launches without restoring NONA as "
      "user intent");

  const fs::path backend_generation_dir = games / "backend-generation";
  fs::create_directories(backend_generation_dir);
  YAML::Node backend_generation_private(YAML::NodeType::Map);
  backend_generation_private["hstream_ui"]["stitching_calibration"]["status"] = "pending";
  backend_generation_private["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "shared-generation-a";
  ok &= expect(
      hm::stitching::publish_game_config(backend_generation_dir, YAML::Dump(backend_generation_private) + "\n").ok(),
      "shared backend-generation fixture must publish");
  hm::Configurator first_backend_generation(
      "backend-generation", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  hm::Configurator second_backend_generation(
      "backend-generation", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status first_generation_configured = first_backend_generation.configure();
  const absl::Status second_generation_configured = second_backend_generation.configure();
  const absl::Status first_generation_backend =
      first_backend_generation.apply_config_item("stitching.mapping_backend", "opencv-magsac");
  const absl::Status first_generation_autooptimizer =
      first_backend_generation.apply_config_item("stitching.run_autooptimizer", "false");
  const absl::Status second_generation_backend =
      second_backend_generation.apply_config_item("stitching.mapping_backend", "opencv-affine-ransac");
  const absl::Status second_generation_autooptimizer =
      second_backend_generation.apply_config_item("stitching.run_autooptimizer", "false");
  g_setenv("HSTREAM_GAME_CONFIG_TEST_INTERRUPT_BEFORE_RENAME", "1", TRUE);
  const absl::Status first_generation_interrupted =
      first_backend_generation.persist_effective_stitching_backend_choices("shared-generation-a");
  g_unsetenv("HSTREAM_GAME_CONFIG_TEST_INTERRUPT_BEFORE_RENAME");
  auto backend_generation_after_interruption =
      hm::stitching::load_game_config_file(backend_generation_dir / "config.yaml");
  const absl::Status first_generation_reserved =
      first_backend_generation.persist_effective_stitching_backend_choices("shared-generation-a");
  const absl::Status second_generation_rejected =
      second_backend_generation.persist_effective_stitching_backend_choices("shared-generation-a");
  auto backend_generation_final = hm::stitching::load_game_config_file(backend_generation_dir / "config.yaml");
  ok &= expect(
      first_generation_configured.ok() && second_generation_configured.ok() && first_generation_backend.ok() &&
          first_generation_autooptimizer.ok() && second_generation_backend.ok() &&
          second_generation_autooptimizer.ok() && !first_generation_interrupted.ok() &&
          backend_generation_after_interruption.ok() && backend_generation_after_interruption->has_value() &&
          !hm::get_node(**backend_generation_after_interruption, "stitching.mapping_backend").has_value() &&
          !hm::get_node(**backend_generation_after_interruption, "hstream_ui.generated_stitching_backend_choices")
               .has_value() &&
          !hm::get_node(**backend_generation_after_interruption, "hstream_ui.stitching_calibration.backend_generation")
               .has_value() &&
          first_generation_reserved.ok() && absl::IsAborted(second_generation_rejected) &&
          backend_generation_final.ok() && backend_generation_final->has_value() &&
          (**backend_generation_final)["stitching"]["mapping_backend"].as<std::string>() == "opencv-magsac" &&
          (**backend_generation_final)["hstream_ui"]["generated_stitching_backend_choices"]["mapping_backend"]
                  .as<std::string>() == "opencv-magsac" &&
          (**backend_generation_final)["hstream_ui"]["stitching_calibration"]["backend_generation"]["mapping_backend"]
                  .as<std::string>() == "opencv-magsac",
      "Backend provenance, worker tuple, and generation claim must publish atomically, and preloaded configurators "
      "must not share one invalidation ID with different backend tuples");

  const fs::path invalid_backend_dir = games / "invalid-backend-cli";
  fs::create_directories(invalid_backend_dir);
  std::ofstream(invalid_backend_dir / "seam_file.png") << "preserved seam\n";
  std::ofstream(invalid_backend_dir / "mapping_0000.tif") << "preserved mapping\n";
  YAML::Node invalid_backend_private(YAML::NodeType::Map);
  invalid_backend_private["pipeline"]["application"]["complete-configuration"] = 1;
  invalid_backend_private["pipeline"]["hmstitcher"]["enable"] = 1;
  invalid_backend_private["stitching"]["mapping_backend"] = "opencv-magsac";
  invalid_backend_private["stitching"]["run_autooptimizer"] = false;
  invalid_backend_private["hstream_ui"]["stitching_calibration"]["control_points"] = 1500;
  invalid_backend_private["hstream_ui"]["stitching_calibration"]["frame_count"] = 1;
  invalid_backend_private["hstream_ui"]["stitching_calibration"]["status"] = "pending";
  invalid_backend_private["hstream_ui"]["stitching_calibration"]["stale_from"] = "input";
  invalid_backend_private["hstream_ui"]["stitching_calibration"]["artifacts_invalidated"] = false;
  invalid_backend_private["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "invalid-backend-a";
  ok &= expect(
      hm::stitching::publish_game_config(invalid_backend_dir, YAML::Dump(invalid_backend_private) + "\n").ok(),
      "invalid backend CLI fixture must publish");
  std::ifstream invalid_backend_before_stream(invalid_backend_dir / "config.yaml", std::ios::binary);
  const std::string invalid_backend_before{
      std::istreambuf_iterator<char>(invalid_backend_before_stream), std::istreambuf_iterator<char>()};
  hm::Configurator invalid_backend_cli(
      "invalid-backend-cli", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status invalid_backend_configured = invalid_backend_cli.configure();
  const absl::Status invalid_backend_option =
      invalid_backend_cli.apply_config_item("stitching.mapping_backend", "nona");
  const absl::Status invalid_backend_autooptimizer =
      invalid_backend_cli.apply_config_item("stitching.run_autooptimizer", "false");
  const absl::Status invalid_backend_status = invalid_backend_cli.complete_configuration(
      /*force=*/false,
      /*clean_stitching_artifacts=*/false,
      /*clean_stitching_from_control_points=*/false,
      /*clean_expected_invalidation_id=*/{},
      /*show_render_sink=*/false,
      /*show_render_scale=*/-1.0);
  std::ifstream invalid_backend_after_stream(invalid_backend_dir / "config.yaml", std::ios::binary);
  const std::string invalid_backend_after{
      std::istreambuf_iterator<char>(invalid_backend_after_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      invalid_backend_configured.ok() && invalid_backend_option.ok() && invalid_backend_autooptimizer.ok() &&
          absl::IsInvalidArgument(invalid_backend_status) &&
          std::string(invalid_backend_status.message()).find("requires stitching.run_autooptimizer=true") !=
              std::string::npos &&
          invalid_backend_after == invalid_backend_before &&
          fs::is_regular_file(invalid_backend_dir / "seam_file.png") &&
          fs::is_regular_file(invalid_backend_dir / "mapping_0000.tif"),
      "A direct NONA-without-autooptimizer override must fail before changing private config or cleaning artifacts");

  const fs::path backend_partial_baseline_root = root / "backend-partial-baseline";
  fs::create_directories(backend_partial_baseline_root);
  YAML::Node backend_partial_baseline = YAML::Clone(test_baseline);
  backend_partial_baseline["stitching"]["mapping_backend"] = "opencv-magsac";
  backend_partial_baseline["stitching"]["projection"] = "general-panini";
  backend_partial_baseline["stitching"]["run_autooptimizer"] = true;
  backend_partial_baseline["stitching"]["projection_parameters"]["general-panini"] = YAML::Load("[100, 0, 0]");
  std::ofstream(backend_partial_baseline_root / "baseline.yaml") << YAML::Dump(backend_partial_baseline) << '\n';
  const fs::path backend_partial_dir = games / "backend-partial";
  fs::create_directories(backend_partial_dir);
  YAML::Node backend_partial_private(YAML::NodeType::Map);
  backend_partial_private["stitching"]["control_point_matcher"] = "superpoint-lightglue";
  backend_partial_private["stitching"]["mapping_backend"] = "nona";
  ok &= expect(
      hm::stitching::publish_game_config(backend_partial_dir, YAML::Dump(backend_partial_private) + "\n").ok(),
      "backend partial fixture must publish");
  hm::Configurator backend_partial(
      "backend-partial", backend_partial_baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status backend_partial_configured = backend_partial.configure();
  const absl::Status backend_partial_option =
      backend_partial.apply_config_item("stitching.mapping_backend", "opencv-magsac");
  const absl::Status backend_partial_persisted = backend_partial.persist_effective_stitching_backend_choices();
  hm::Configurator backend_partial_reloaded(
      "backend-partial", backend_partial_baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status backend_partial_reconfigured = backend_partial_reloaded.configure();
  const absl::Status backend_partial_restored = backend_partial_reloaded.persist_effective_stitching_backend_choices();
  auto backend_partial_final = hm::stitching::load_game_config_file(backend_partial_dir / "config.yaml");
  ok &= expect(
      backend_partial_configured.ok() && backend_partial_option.ok() && backend_partial_persisted.ok() &&
          backend_partial_reconfigured.ok() && backend_partial_restored.ok() && backend_partial_final.ok() &&
          backend_partial_final->has_value() &&
          (**backend_partial_final)["stitching"]["control_point_matcher"].as<std::string>() == "superpoint-lightglue" &&
          (**backend_partial_final)["stitching"]["mapping_backend"].as<std::string>() == "nona" &&
          (**backend_partial_final)["stitching"]["projection"].as<std::string>() == "general-panini" &&
          (**backend_partial_final)["stitching"]["run_autooptimizer"].as<bool>() &&
          (**backend_partial_final)["hstream_ui"]["generated_stitching_backend_choices"]
                                   ["previous_control_point_matcher"]
                                       .as<std::string>() == "superpoint-lightglue" &&
          (**backend_partial_final)["hstream_ui"]["generated_stitching_backend_choices"]["previous_mapping_backend"]
                  .as<std::string>() == "nona" &&
          !hm::get_node(
               **backend_partial_final,
               "hstream_ui.generated_stitching_backend_choices.previous_projection")
               .has_value() &&
          !hm::get_node(
               **backend_partial_final,
               "hstream_ui.generated_stitching_backend_choices.previous_run_autooptimizer")
               .has_value(),
      "Restoring a partial private backend choice must keep the complete effective worker tuple materialized and its "
      "provenance intact");
  backend_partial_baseline["stitching"]["projection_parameters"]["general-panini"][0] = 120;
  std::ofstream(backend_partial_baseline_root / "baseline.yaml") << YAML::Dump(backend_partial_baseline) << '\n';
  hm::Configurator backend_partial_parameter_changed(
      "backend-partial", backend_partial_baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status backend_partial_parameter_configured = backend_partial_parameter_changed.configure();
  const absl::Status backend_partial_parameter_persisted =
      backend_partial_parameter_changed.persist_effective_stitching_backend_choices();
  auto backend_partial_parameter_final = hm::stitching::load_game_config_file(backend_partial_dir / "config.yaml");
  ok &= expect(
      backend_partial_parameter_configured.ok() && backend_partial_parameter_persisted.ok() &&
          backend_partial_parameter_final.ok() && backend_partial_parameter_final->has_value() &&
          (**backend_partial_parameter_final)["stitching"]["projection_parameters"]["general-panini"][0].as<double>() ==
              120.0 &&
          (**backend_partial_parameter_final)["hstream_ui"]["generated_stitching_backend_choices"]
                                             ["projection_parameters"][0]
                                                 .as<double>() == 120.0,
      "Generated projection-parameter provenance must continue inheriting lower-layer General Panini changes");
  backend_partial_baseline["stitching"]["projection"] = "cylindrical";
  std::ofstream(backend_partial_baseline_root / "baseline.yaml") << YAML::Dump(backend_partial_baseline) << '\n';
  hm::Configurator backend_partial_changed(
      "backend-partial", backend_partial_baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status backend_partial_changed_configured = backend_partial_changed.configure();
  const absl::Status backend_partial_changed_persisted =
      backend_partial_changed.persist_effective_stitching_backend_choices();
  auto backend_partial_changed_final = hm::stitching::load_game_config_file(backend_partial_dir / "config.yaml");
  ok &= expect(
      backend_partial_changed_configured.ok() && backend_partial_changed_persisted.ok() &&
          backend_partial_changed_final.ok() && backend_partial_changed_final->has_value() &&
          (**backend_partial_changed_final)["stitching"]["mapping_backend"].as<std::string>() == "nona" &&
          (**backend_partial_changed_final)["stitching"]["projection"].as<std::string>() == "cylindrical" &&
          (**backend_partial_changed_final)["hstream_ui"]["generated_stitching_backend_choices"]["projection"]
                  .as<std::string>() == "cylindrical",
      "Restored partial private choices must continue inheriting later lower-layer projection changes");

  backend_partial_baseline["stitching"]["projection"] = "general-panini";
  std::ofstream(backend_partial_baseline_root / "baseline.yaml") << YAML::Dump(backend_partial_baseline) << '\n';
  const fs::path inherited_parameter_dir = games / "inherited-projection-private-parameters";
  fs::create_directories(inherited_parameter_dir);
  YAML::Node inherited_parameter_private(YAML::NodeType::Map);
  inherited_parameter_private["stitching"]["control_point_matcher"] = "superpoint-lightglue";
  inherited_parameter_private["stitching"]["mapping_backend"] = "nona";
  inherited_parameter_private["stitching"]["projection"] = "general_panini";
  inherited_parameter_private["stitching"]["projection_parameters"]["general-panini"] = YAML::Load("[110, 5, -5]");
  ok &= expect(
      hm::stitching::publish_game_config(inherited_parameter_dir, YAML::Dump(inherited_parameter_private) + "\n").ok(),
      "inherited projection parameter fixture must publish");
  hm::Configurator inherited_parameter_first(
      "inherited-projection-private-parameters",
      backend_partial_baseline_root.string(),
      hm::Configurator::kUseConfigFileGpu);
  const absl::Status inherited_parameter_first_configured = inherited_parameter_first.configure();
  const absl::Status inherited_parameter_first_persisted =
      inherited_parameter_first.persist_effective_stitching_backend_choices();
  hm::Configurator inherited_parameter_reloaded(
      "inherited-projection-private-parameters",
      backend_partial_baseline_root.string(),
      hm::Configurator::kUseConfigFileGpu);
  const absl::Status inherited_parameter_reconfigured = inherited_parameter_reloaded.configure();
  const absl::Status inherited_parameter_restored =
      inherited_parameter_reloaded.persist_effective_stitching_backend_choices();
  auto inherited_parameter_final = hm::stitching::load_game_config_file(inherited_parameter_dir / "config.yaml");
  ok &= expect(
      inherited_parameter_first_configured.ok() && inherited_parameter_first_persisted.ok() &&
          inherited_parameter_reconfigured.ok() && inherited_parameter_restored.ok() &&
          inherited_parameter_final.ok() && inherited_parameter_final->has_value() &&
          (**inherited_parameter_final)["stitching"]["projection_parameters"]["general-panini"][0].as<double>() ==
              110.0 &&
          (**inherited_parameter_final)["hstream_ui"]["generated_stitching_backend_choices"]["projection_parameters"][0]
                  .as<double>() == 110.0 &&
          (**inherited_parameter_final)["hstream_ui"]["generated_stitching_backend_choices"]
                                       ["previous_projection_parameters"][0]
                                           .as<double>() == 110.0 &&
          (**inherited_parameter_final)["hstream_ui"]["generated_stitching_backend_choices"]["previous_projection"]
                  .as<std::string>() == "general_panini" &&
          !hm::get_node(**inherited_parameter_final, "stitching.projection_parameters.general_panini").has_value(),
      "Private parameters under the canonical key for an aliased projection scalar must survive generated-choice "
      "normalization and reload");

  const fs::path legacy_opencv_dir = games / "legacy-opencv-no-projection";
  fs::create_directories(legacy_opencv_dir);
  YAML::Node legacy_opencv_private(YAML::NodeType::Map);
  legacy_opencv_private["stitching"]["control_point_matcher"] = "superpoint-lightglue";
  legacy_opencv_private["stitching"]["mapping_backend"] = "opencv-affine-ransac";
  ok &= expect(
      hm::stitching::publish_game_config(legacy_opencv_dir, YAML::Dump(legacy_opencv_private) + "\n").ok(),
      "legacy OpenCV projection migration fixture must publish");
  hm::Configurator legacy_opencv_first(
      "legacy-opencv-no-projection", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status legacy_opencv_first_configured = legacy_opencv_first.configure();
  const absl::Status legacy_opencv_first_persisted = legacy_opencv_first.persist_effective_stitching_backend_choices();
  auto legacy_opencv_after_first = hm::stitching::load_game_config_file(legacy_opencv_dir / "config.yaml");
  hm::Configurator legacy_opencv_second(
      "legacy-opencv-no-projection", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status legacy_opencv_second_configured = legacy_opencv_second.configure();
  const absl::Status legacy_opencv_second_persisted =
      legacy_opencv_second.persist_effective_stitching_backend_choices();
  auto legacy_opencv_after_second = hm::stitching::load_game_config_file(legacy_opencv_dir / "config.yaml");
  hm::Configurator legacy_opencv_third(
      "legacy-opencv-no-projection", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  const absl::Status legacy_opencv_third_configured = legacy_opencv_third.configure();
  const absl::Status legacy_opencv_third_persisted = legacy_opencv_third.persist_effective_stitching_backend_choices();
  auto legacy_opencv_after_third = hm::stitching::load_game_config_file(legacy_opencv_dir / "config.yaml");
  ok &= expect(
      legacy_opencv_first_configured.ok() && legacy_opencv_first_persisted.ok() && legacy_opencv_after_first.ok() &&
          legacy_opencv_after_first->has_value() &&
          (**legacy_opencv_after_first)["stitching"]["projection"].as<std::string>() == "rectilinear" &&
          legacy_opencv_second_configured.ok() && legacy_opencv_second_persisted.ok() &&
          legacy_opencv_after_second.ok() && legacy_opencv_after_second->has_value() &&
          (**legacy_opencv_after_second)["stitching"]["mapping_backend"].as<std::string>() == "opencv-affine-ransac" &&
          (**legacy_opencv_after_second)["stitching"]["projection"].as<std::string>() == "rectilinear" &&
          hm::get_node(**legacy_opencv_after_second, "hstream_ui.generated_stitching_backend_choices").has_value() &&
          legacy_opencv_third_configured.ok() && legacy_opencv_third_persisted.ok() && legacy_opencv_after_third.ok() &&
          legacy_opencv_after_third->has_value() &&
          (**legacy_opencv_after_third)["stitching"]["mapping_backend"].as<std::string>() == "opencv-affine-ransac" &&
          (**legacy_opencv_after_third)["stitching"]["projection"].as<std::string>() == "rectilinear" &&
          hm::get_node(**legacy_opencv_after_third, "hstream_ui.generated_stitching_backend_choices").has_value(),
      "Legacy OpenCV configs without projection must keep a stable rectilinear worker tuple and provenance for "
      "inherited choices");

  YAML::Node source_uri_spellings(YAML::NodeType::Map);
  source_uri_spellings["source0"]["enable"] = 1;
  source_uri_spellings["source0"]["type"] = static_cast<int>(NV_DS_SOURCE_URI_MULTIPLE);
  source_uri_spellings["source0"]["uri_list"].push_back("file:///camera/chapter-1.mp4");
  source_uri_spellings["source0"]["uri_list"].push_back("file:///camera/chapter-highest-bitrate.mp4");
  source_uri_spellings["source0"]["uri"] = "file:///camera/chapter-1.mp4";
  source_uri_spellings["source1"]["enable"] = 0;
  source_uri_spellings["source1"]["type"] = static_cast<int>(NV_DS_SOURCE_URI_MULTIPLE);
  source_uri_spellings["source1"]["uri-list"].push_back("file:///disabled.mp4");
  const auto source_uris = hm::configurator_internal::enabled_source_video_uris(source_uri_spellings);
  ok &= expect(
      source_uris ==
          std::vector<std::string>{
              "file:///camera/chapter-1.mp4",
              "file:///camera/chapter-highest-bitrate.mp4",
              "file:///camera/chapter-1.mp4"},
      "Bitrate discovery must inspect every enabled uri_list chapter, including later higher-bitrate entries");

  YAML::Node explicit_roles(YAML::NodeType::Map);
  explicit_roles["hstream_ui"]["video_roles"]["left"].push_back(".hstream-ui/left/GX010001.MP4");
  explicit_roles["hstream_ui"]["video_roles"]["right"].push_back(".hstream-ui/right/GX010002.MP4");
  auto selected = hm::configurator_internal::select_explicit_stitching_videos(explicit_roles, /*force=*/true);
  ok &= expect(
      selected.ui_roles_are_authoritative && selected.left_is_explicit && selected.right_is_explicit &&
          selected.left == std::vector<std::string>{".hstream-ui/left/GX010001.MP4"} &&
          selected.right == std::vector<std::string>{".hstream-ui/right/GX010002.MP4"},
      "Forced configuration must reconstruct missing runtime video lists from explicit UI roles");

  explicit_roles["game"]["videos"]["left"].push_back("cam2/GX010002.MP4");
  explicit_roles["game"]["videos"]["right"].push_back("cam1/GX010001.MP4");
  selected = hm::configurator_internal::select_explicit_stitching_videos(explicit_roles, /*force=*/true);
  ok &= expect(
      selected.left == std::vector<std::string>{".hstream-ui/left/GX010001.MP4"} &&
          selected.right == std::vector<std::string>{".hstream-ui/right/GX010002.MP4"},
      "Forced configuration must prefer explicit UI roles over mismatched derived runtime lists");

  YAML::Node out_of_order_left(YAML::NodeType::Sequence);
  out_of_order_left.push_back(".hstream-ui/left/GX020001.MP4");
  out_of_order_left.push_back(".hstream-ui/left/GX010001.MP4");
  explicit_roles["hstream_ui"]["video_roles"]["left"] = out_of_order_left;
  explicit_roles["hstream_ui"]["video_roles"]["right"].push_back(".hstream-ui/right/GX020002.MP4");
  selected = hm::configurator_internal::select_explicit_stitching_videos(explicit_roles, /*force=*/true);
  ok &= expect(
      selected.error.empty() &&
          selected.left == std::vector<std::string>{".hstream-ui/left/GX010001.MP4", ".hstream-ui/left/GX020001.MP4"} &&
          selected.right ==
              std::vector<std::string>{".hstream-ui/right/GX010002.MP4", ".hstream-ui/right/GX020002.MP4"},
      "Explicit UI Left/Right roles must be paired and sorted by chapter");

  explicit_roles["hstream_ui"]["video_roles"]["right"][1] = ".hstream-ui/right/GX030002.MP4";
  selected = hm::configurator_internal::select_explicit_stitching_videos(explicit_roles, /*force=*/true);
  ok &= expect(
      selected.error.empty() &&
          selected.left == std::vector<std::string>{".hstream-ui/left/GX010001.MP4", ".hstream-ui/left/GX020001.MP4"} &&
          selected.right ==
              std::vector<std::string>{".hstream-ui/right/GX010002.MP4", ".hstream-ui/right/GX030002.MP4"},
      "Explicit camera playlists must not be paired by physical chapter number");

  explicit_roles["hstream_ui"]["video_roles"]["right"].push_back(".hstream-ui/right/GX020002.MP4");
  selected = hm::configurator_internal::select_explicit_stitching_videos(explicit_roles, /*force=*/true);
  ok &= expect(
      selected.error.empty() && selected.left.size() == 2 && selected.right.size() == 3 &&
          selected.right[1] == ".hstream-ui/right/GX020002.MP4" &&
          selected.right[2] == ".hstream-ui/right/GX030002.MP4",
      "Explicit camera playlists with different physical chapter counts must be retained independently");

  YAML::Node saved_uneven_playlists(YAML::NodeType::Map);
  saved_uneven_playlists["game"]["videos"]["left"] = selected.left;
  saved_uneven_playlists["game"]["videos"]["right"] = selected.right;
  const auto saved_selection =
      hm::configurator_internal::select_explicit_stitching_videos(saved_uneven_playlists, /*force=*/false);
  ok &= expect(
      saved_selection.error.empty() && saved_selection.left.size() == 2 && saved_selection.right.size() == 3,
      "A subsequent run must accept independently persisted camera playlists");

  YAML::Node independent_name_schemes(YAML::NodeType::Map);
  independent_name_schemes["hstream_ui"]["video_roles"]["left"] = out_of_order_left;
  independent_name_schemes["hstream_ui"]["video_roles"]["right"].push_back("right-camera.mov");
  independent_name_schemes["hstream_ui"]["video_roles"]["right"].push_back("right-camera-alt.mov");
  const auto independently_named =
      hm::configurator_internal::select_explicit_stitching_videos(independent_name_schemes, /*force=*/true);
  ok &= expect(
      independently_named.error.empty() &&
          independently_named.left ==
              std::vector<std::string>{".hstream-ui/left/GX010001.MP4", ".hstream-ui/left/GX020001.MP4"} &&
          independently_named.right == std::vector<std::string>{"right-camera.mov", "right-camera-alt.mov"},
      "Each explicit camera playlist must use its own valid naming and ordering scheme");

  YAML::Node one_sided_out_of_order(YAML::NodeType::Map);
  one_sided_out_of_order["hstream_ui"]["video_roles"]["left"] = out_of_order_left;
  const auto normalized_one_sided =
      hm::configurator_internal::select_explicit_stitching_videos(one_sided_out_of_order, /*force=*/true);
  ok &= expect(
      normalized_one_sided.error.empty() && normalized_one_sided.left_is_explicit &&
          !normalized_one_sided.right_is_explicit &&
          normalized_one_sided.left ==
              std::vector<std::string>{".hstream-ui/left/GX010001.MP4", ".hstream-ui/left/GX020001.MP4"},
      "A one-sided explicit playlist must be normalized before the unambiguous single-file Auto mode uses it");

  YAML::Node restarted_recordings(YAML::NodeType::Map);
  restarted_recordings["hstream_ui"]["video_roles"]["left"].push_back("GX010002.MP4");
  restarted_recordings["hstream_ui"]["video_roles"]["left"].push_back("GX020001.MP4");
  restarted_recordings["hstream_ui"]["video_roles"]["right"].push_back("VID_20260815_101500_001.MP4");
  restarted_recordings["hstream_ui"]["video_roles"]["right"].push_back("VID_20260815_101000_002.MP4");
  const auto normalized_restarts =
      hm::configurator_internal::select_explicit_stitching_videos(restarted_recordings, /*force=*/true);
  ok &= expect(
      normalized_restarts.error.empty() &&
          normalized_restarts.left == std::vector<std::string>{"GX020001.MP4", "GX010002.MP4"} &&
          normalized_restarts.right ==
              std::vector<std::string>{"VID_20260815_101000_002.MP4", "VID_20260815_101500_001.MP4"},
      "Explicit GoPro and Insta360 playlists must sort by recording ID before physical chapter number");

  YAML::Node duplicate_arbitrary(YAML::NodeType::Map);
  duplicate_arbitrary["hstream_ui"]["video_roles"]["left"].push_back("left-camera.mov");
  duplicate_arbitrary["hstream_ui"]["video_roles"]["left"].push_back("left-camera.mov");
  const auto rejected_duplicate =
      hm::configurator_internal::select_explicit_stitching_videos(duplicate_arbitrary, /*force=*/true);
  ok &= expect(
      !rejected_duplicate.error.empty(), "An exact duplicate path must never replay the same arbitrary-named file");

  YAML::Node numbered_parts(YAML::NodeType::Map);
  numbered_parts["hstream_ui"]["video_roles"]["left"].push_back("left-12.mkv");
  numbered_parts["hstream_ui"]["video_roles"]["left"].push_back("left-2.mkv");
  const auto normalized_parts =
      hm::configurator_internal::select_explicit_stitching_videos(numbered_parts, /*force=*/true);
  ok &= expect(
      normalized_parts.error.empty() && normalized_parts.left == std::vector<std::string>{"left-2.mkv", "left-12.mkv"},
      "Explicit left/right part playlists must support Auto formats and sort multi-digit parts numerically");

  YAML::Node heterogeneous_camera(YAML::NodeType::Map);
  heterogeneous_camera["hstream_ui"]["video_roles"]["left"].push_back("VID_20260815_101000_001.MP4");
  heterogeneous_camera["hstream_ui"]["video_roles"]["left"].push_back("GX010007.MP4");
  heterogeneous_camera["hstream_ui"]["video_roles"]["left"].push_back("left-camera.mov");
  const auto preserved_heterogeneous =
      hm::configurator_internal::select_explicit_stitching_videos(heterogeneous_camera, /*force=*/true);
  ok &= expect(
      preserved_heterogeneous.error.empty() &&
          preserved_heterogeneous.left ==
              std::vector<std::string>{"VID_20260815_101000_001.MP4", "GX010007.MP4", "left-camera.mov"},
      "A heterogeneous explicit camera playlist must preserve the user's total recording order");

  YAML::Node duplicate_persisted(YAML::NodeType::Map);
  duplicate_persisted["game"]["videos"]["left"].push_back("cam1/GX010001.MP4");
  duplicate_persisted["game"]["videos"]["left"].push_back("cam1/GX010001.MP4");
  duplicate_persisted["game"]["videos"]["right"].push_back("cam2/GX010002.MP4");
  const auto rejected_persisted =
      hm::configurator_internal::select_explicit_stitching_videos(duplicate_persisted, /*force=*/false);
  ok &= expect(
      !rejected_persisted.error.empty(), "A persisted camera playlist must never replay an exact duplicate path");

  ok &= expect(
      !hm::configurator_internal::validate_mixed_explicit_auto_playlists(
           /*left_is_explicit=*/true,
           /*right_is_explicit=*/false,
           /*auto_left_chapters=*/0,
           /*auto_right_chapters=*/3)
              .ok() &&
          !hm::configurator_internal::validate_mixed_explicit_auto_playlists(
               /*left_is_explicit=*/false,
               /*right_is_explicit=*/true,
               /*auto_left_chapters=*/2,
               /*auto_right_chapters=*/0)
               .ok(),
      "Mixed Explicit/Auto selection must reject an ambiguous multi-file Auto camera timeline");
  ok &= expect(
      hm::configurator_internal::validate_mixed_explicit_auto_playlists(
          /*left_is_explicit=*/true,
          /*right_is_explicit=*/false,
          /*auto_left_chapters=*/0,
          /*auto_right_chapters=*/1)
              .ok() &&
          hm::configurator_internal::validate_mixed_explicit_auto_playlists(
              /*left_is_explicit=*/true,
              /*right_is_explicit=*/true,
              /*auto_left_chapters=*/4,
              /*auto_right_chapters=*/6)
              .ok(),
      "A single-file Auto side and two independently explicit playlists are unambiguous");

  const fs::path custom_archive_dir = root / "configured-output" / "custom-archive-game";
  const fs::path custom_archive = custom_archive_dir / "operator-selected-name.mkv";
  const fs::path custom_recovery = custom_archive_dir / "operator-selected-name-finalization-failed.mkv";
  fs::create_directories(custom_archive_dir);
  std::ofstream(custom_archive, std::ios::binary) << "completed data from an interrupted custom archive";
  const auto recovered_custom_archive = hm::configurator_internal::preserve_existing_archive_work_file(custom_archive);
  std::ifstream recovered_input(custom_recovery, std::ios::binary);
  const std::string recovered_content{
      std::istreambuf_iterator<char>(recovered_input), std::istreambuf_iterator<char>()};
  ok &= expect(
      recovered_custom_archive.ok() && recovered_custom_archive->has_value() &&
          recovered_custom_archive->value() == custom_recovery && !fs::exists(custom_archive) &&
          recovered_content == "completed data from an interrupted custom archive",
      "Backend output configuration must durably preserve a nonempty custom work path before sink construction");

  const auto first_unique_work =
      hm::configurator_internal::reserve_unique_archive_work_file(custom_archive, "1234-review-run-a");
  const auto duplicate_unique_work =
      hm::configurator_internal::reserve_unique_archive_work_file(custom_archive, "1234-review-run-a");
  const auto second_unique_work =
      hm::configurator_internal::reserve_unique_archive_work_file(custom_archive, "5678-review-run-b");
  ok &= expect(
      first_unique_work.ok() && second_unique_work.ok() && *first_unique_work != *second_unique_work &&
          fs::is_regular_file(*first_unique_work) && fs::is_regular_file(*second_unique_work) &&
          duplicate_unique_work.status().code() == absl::StatusCode::kAlreadyExists,
      "Concurrent UI archive runs must reserve different work paths and never share or overwrite one another");

  const auto failed_unique_reservation = hm::configurator_internal::reserve_unique_archive_work_file(
      root / "missing-archive-parent" / "archive.mkv", "9999-no-parent");
  ok &= expect(
      !failed_unique_reservation.ok() && failed_unique_reservation.status().code() == absl::StatusCode::kInternal &&
          failed_unique_reservation.status().message().find("No such file or directory") != std::string::npos,
      "Unique work reservation must preserve non-collision errno diagnostics");

  std::map<std::string, std::string> claimed_archive_paths;
  const auto first_archive_claim =
      hm::configurator_internal::claim_unique_archive_output_path(claimed_archive_paths, custom_archive, "sink0");
  const auto duplicate_archive_claim = hm::configurator_internal::claim_unique_archive_output_path(
      claimed_archive_paths, custom_archive_dir / "." / custom_archive.filename(), "sink1");
  ok &= expect(
      first_archive_claim.ok() && duplicate_archive_claim.code() == absl::StatusCode::kInvalidArgument &&
          duplicate_archive_claim.message().find("sink0") != std::string::npos &&
          duplicate_archive_claim.message().find("sink1") != std::string::npos,
      "Two enabled encode sinks must never be allowed to write the same normalized archive output");

  const fs::path stale_run = custom_archive_dir / "operator-selected-name.hstream-run-99999999-88888888-dead.mkv";
  const fs::path stale_run_log = stale_run.string() + ".log";
  const fs::path stale_versioned_run = custom_archive_dir /
      ("operator-selected-name.hstream-run-v2-99999998-" + std::to_string(::getpid()) +
       "-01234567-89ab-cdef-0123-456789abcdef.mkv");
  const fs::path stale_pre_version_run = custom_archive_dir /
      ("operator-selected-name.hstream-run-99999997-" + std::to_string(::getpid()) +
       "-fedcba98-7654-3210-fedc-ba9876543210.mkv");
  const fs::path dead_versioned_run = custom_archive_dir /
      "operator-selected-name.hstream-run-v2-99999996-99999995-abcdef01-2345-6789-abcd-ef0123456789.mkv";
  const fs::path dead_pre_version_run = custom_archive_dir /
      "operator-selected-name.hstream-run-99999994-99999993-10fedcba-9876-5432-10fe-dcba98765432.mkv";
  const fs::path live_run = custom_archive_dir /
      ("operator-selected-name.hstream-run-" + std::to_string(::getpid()) + "-" + std::to_string(::getpid()) +
       "-live.mkv");
  const fs::path legacy_live_ui_run = custom_archive_dir /
      ("operator-selected-name.hstream-run-" + std::to_string(::getpid()) +
       "-00112233-4455-6677-8899-aabbccddeeff.mkv");
  std::ofstream(stale_run, std::ios::binary) << "stale unique run data";
  std::ofstream(stale_run_log, std::ios::binary) << "stale unique run log";
  std::ofstream(stale_versioned_run, std::ios::binary) << "stale versioned run with live UI parent";
  std::ofstream(stale_pre_version_run, std::ios::binary) << "stale pre-version run with live UI parent";
  std::ofstream(dead_versioned_run, std::ios::binary) << "stale versioned run with dead backend and UI";
  std::ofstream(dead_pre_version_run, std::ios::binary) << "stale pre-version run with dead backend and UI";
  std::ofstream(live_run, std::ios::binary) << "live unique run data";
  std::ofstream(legacy_live_ui_run, std::ios::binary) << "legacy work owned by live UI";
  const auto stale_recoveries = hm::configurator_internal::recover_stale_archive_work_files(custom_archive);
  bool stale_log_recovered_with_video = false;
  if (stale_recoveries.ok()) {
    for (const fs::path& recovery : *stale_recoveries) {
      std::ifstream recovered_log(recovery.string() + ".log", std::ios::binary);
      const std::string recovered_log_content{
          std::istreambuf_iterator<char>(recovered_log), std::istreambuf_iterator<char>()};
      stale_log_recovered_with_video =
          stale_log_recovered_with_video || recovered_log_content == "stale unique run log";
    }
  }
  ok &= expect(
      stale_recoveries.ok() && stale_recoveries->size() == 3 && !fs::exists(stale_run) && !fs::exists(stale_run_log) &&
          stale_log_recovered_with_video && fs::is_regular_file(stale_versioned_run) &&
          fs::is_regular_file(stale_pre_version_run) && !fs::exists(dead_versioned_run) &&
          !fs::exists(dead_pre_version_run) &&
          std::all_of(
              stale_recoveries->begin(),
              stale_recoveries->end(),
              [](const fs::path& path) {
                return fs::is_regular_file(path) && !fs::exists(path.string() + ".hstream-pin") &&
                    !fs::exists(path.string() + ".log.hstream-pin");
              }) &&
          fs::is_regular_file(live_run) && fs::is_regular_file(legacy_live_ui_run),
      "Restart recovery must pair each stale job log with its recovered video while retaining work whose owner is alive");
  const auto repeated_stale_recoveries = hm::configurator_internal::recover_stale_archive_work_files(custom_archive);
  ok &= expect(
      repeated_stale_recoveries.ok() && repeated_stale_recoveries->empty() && stale_recoveries.ok() &&
          fs::is_regular_file(stale_recoveries->front()),
      "Restart recovery must leave an already-recovered unique run at its stable recovery path");

  const fs::path provisional_log_dir = root / "archive-provisional-log";
  fs::create_directories(provisional_log_dir);
  const fs::path provisional_configured = provisional_log_dir / "provisional.mkv";
  const std::string provisional_run_id = "88888888-00112233-4455-6677-8899-aabbccddeeff";
  const fs::path provisional_source =
      provisional_log_dir / ("provisional.hstream-run-v3-99999999-" + provisional_run_id + ".mkv");
  const fs::path provisional_log =
      provisional_log_dir / ("provisional.hstream-run-ui-" + provisional_run_id + ".mkv.log");
  const fs::path provisional_resolved_log = provisional_source.string() + ".log";
  const fs::path provisional_recovery = provisional_log_dir / "provisional-finalization-failed.mkv";
  std::ofstream(provisional_source, std::ios::binary) << "video before UI path resolution";
  std::ofstream(provisional_log, std::ios::binary) << "provisional UI log";
  std::ofstream(provisional_resolved_log, std::ios::binary) << "foreign resolved log";
  fs::create_hard_link(provisional_source, provisional_source.string() + ".hstream-pin");
  fs::create_hard_link(provisional_log, provisional_log.string() + ".hstream-pin");
  const auto provisional_recoveries =
      hm::configurator_internal::recover_stale_archive_work_files(provisional_configured);
  std::ifstream provisional_recovered_log(provisional_recovery.string() + ".log", std::ios::binary);
  const std::string provisional_recovered_log_content{
      std::istreambuf_iterator<char>(provisional_recovered_log), std::istreambuf_iterator<char>()};
  std::ifstream provisional_foreign_log_stream(provisional_resolved_log, std::ios::binary);
  const std::string provisional_foreign_log_content{
      std::istreambuf_iterator<char>(provisional_foreign_log_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      provisional_recoveries.ok() && provisional_recoveries->size() == 1 &&
          provisional_recoveries->front() == provisional_recovery && !fs::exists(provisional_source) &&
          !fs::exists(provisional_log) && !fs::exists(provisional_source.string() + ".hstream-pin") &&
          !fs::exists(provisional_log.string() + ".hstream-pin") && fs::is_regular_file(provisional_recovery) &&
          !fs::exists(provisional_recovery.string() + ".hstream-pin") &&
          !fs::exists(provisional_recovery.string() + ".log.hstream-pin") &&
          provisional_recovered_log_content == "provisional UI log" &&
          provisional_foreign_log_content == "foreign resolved log",
      "Stale recovery must prefer the guarded provisional UI log and leave a foreign resolved sidecar untouched");

  const fs::path dual_guarded_dir = root / "archive-dual-guarded-log";
  fs::create_directories(dual_guarded_dir);
  const fs::path dual_guarded_configured = dual_guarded_dir / "dual.mkv";
  const std::string dual_guarded_run_id = "77777777-00112233-4455-6677-8899-aabbccddeeff";
  const fs::path dual_guarded_source =
      dual_guarded_dir / ("dual.hstream-run-v3-99999999-" + dual_guarded_run_id + ".mkv");
  const fs::path dual_guarded_resolved_log = dual_guarded_source.string() + ".log";
  const fs::path dual_guarded_provisional_log =
      dual_guarded_dir / ("dual.hstream-run-ui-" + dual_guarded_run_id + ".mkv.log");
  std::ofstream(dual_guarded_source, std::ios::binary) << "dual guarded video";
  std::ofstream(dual_guarded_resolved_log, std::ios::binary) << "newer resolved log";
  std::ofstream(dual_guarded_provisional_log, std::ios::binary) << "older provisional log";
  fs::create_hard_link(dual_guarded_source, dual_guarded_source.string() + ".hstream-pin");
  fs::create_hard_link(dual_guarded_resolved_log, dual_guarded_resolved_log.string() + ".hstream-pin");
  fs::create_hard_link(dual_guarded_provisional_log, dual_guarded_provisional_log.string() + ".hstream-pin");
  const auto dual_guarded_recovery =
      hm::configurator_internal::recover_stale_archive_work_files(dual_guarded_configured);
  ok &= expect(
      !dual_guarded_recovery.ok() && dual_guarded_recovery.status().code() == absl::StatusCode::kFailedPrecondition &&
          fs::is_regular_file(dual_guarded_source) && fs::is_regular_file(dual_guarded_resolved_log) &&
          fs::is_regular_file(dual_guarded_provisional_log),
      "Stale recovery must reject distinct guarded provisional and resolved logs instead of pairing stale contents");

  const fs::path log_collision_dir = root / "archive-log-collision";
  fs::create_directories(log_collision_dir);
  const fs::path log_collision_archive = log_collision_dir / "collision.mkv";
  const fs::path log_collision_stale = log_collision_dir / "collision.hstream-run-99999999-dead.mkv";
  const fs::path occupied_video = log_collision_dir / "collision-finalization-failed.mkv";
  const fs::path occupied_log = log_collision_dir / "collision-finalization-failed.mkv.log";
  const fs::path expected_collision_recovery = log_collision_dir / "collision-finalization-failed-1.mkv";
  std::ofstream(log_collision_stale, std::ios::binary) << "legacy work without a log";
  std::ofstream(occupied_log, std::ios::binary) << "unrelated existing log";
  const auto collision_recoveries = hm::configurator_internal::recover_stale_archive_work_files(log_collision_archive);
  std::ifstream occupied_log_stream(occupied_log, std::ios::binary);
  const std::string occupied_log_content{
      std::istreambuf_iterator<char>(occupied_log_stream), std::istreambuf_iterator<char>()};
  std::ifstream collision_recovery_stream(expected_collision_recovery, std::ios::binary);
  const std::string collision_recovery_content{
      std::istreambuf_iterator<char>(collision_recovery_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      collision_recoveries.ok() && collision_recoveries->size() == 1 &&
          collision_recoveries->front() == expected_collision_recovery && !fs::exists(log_collision_stale) &&
          !fs::exists(occupied_video) && collision_recovery_content == "legacy work without a log" &&
          !fs::exists(expected_collision_recovery.string() + ".log") &&
          occupied_log_content == "unrelated existing log",
      "Recovery of a video without a log must skip basenames occupied by unrelated log sidecars");

  const fs::path orphan_guard_dir = root / "archive-orphan-guard-collision";
  fs::create_directories(orphan_guard_dir);
  const fs::path orphan_guard_archive = orphan_guard_dir / "orphan.mkv";
  const fs::path orphan_guard_stale = orphan_guard_dir / "orphan.hstream-run-99999999-dead.mkv";
  const fs::path orphan_guard_stale_log = orphan_guard_stale.string() + ".log";
  const fs::path orphan_guard_occupied = orphan_guard_dir / "orphan-finalization-failed.mkv";
  const fs::path orphan_guard_occupied_log = orphan_guard_occupied.string() + ".log";
  const fs::path orphan_guard_expected = orphan_guard_dir / "orphan-finalization-failed-1.mkv";
  const fs::path orphan_guard_expected_log = orphan_guard_expected.string() + ".log";
  const fs::path orphan_guard_backing = orphan_guard_dir / "prior-video-backing";
  const fs::path orphan_log_guard_backing = orphan_guard_dir / "prior-log-backing";
  std::ofstream(orphan_guard_stale, std::ios::binary) << "new stale video";
  std::ofstream(orphan_guard_stale_log, std::ios::binary) << "new stale log";
  std::ofstream(orphan_guard_backing, std::ios::binary) << "prior guarded video";
  std::ofstream(orphan_log_guard_backing, std::ios::binary) << "prior guarded log";
  fs::create_hard_link(orphan_guard_backing, orphan_guard_occupied.string() + ".hstream-pin");
  fs::create_hard_link(orphan_log_guard_backing, orphan_guard_occupied_log.string() + ".hstream-pin");
  const auto orphan_guard_recoveries =
      hm::configurator_internal::recover_stale_archive_work_files(orphan_guard_archive);
  std::ifstream orphan_guard_recovery_stream(orphan_guard_occupied, std::ios::binary);
  const std::string orphan_guard_recovery_content{
      std::istreambuf_iterator<char>(orphan_guard_recovery_stream), std::istreambuf_iterator<char>()};
  std::ifstream orphan_guard_recovery_log_stream(orphan_guard_occupied_log, std::ios::binary);
  const std::string orphan_guard_recovery_log_content{
      std::istreambuf_iterator<char>(orphan_guard_recovery_log_stream), std::istreambuf_iterator<char>()};
  std::ifstream prior_guard_recovery_stream(orphan_guard_expected, std::ios::binary);
  const std::string prior_guard_recovery_content{
      std::istreambuf_iterator<char>(prior_guard_recovery_stream), std::istreambuf_iterator<char>()};
  std::ifstream prior_guard_recovery_log_stream(orphan_guard_expected_log, std::ios::binary);
  const std::string prior_guard_recovery_log_content{
      std::istreambuf_iterator<char>(prior_guard_recovery_log_stream), std::istreambuf_iterator<char>()};
  const bool orphan_guard_recovery_ok = orphan_guard_recoveries.ok() && orphan_guard_recoveries->size() == 2 &&
      orphan_guard_recoveries->at(0) == orphan_guard_occupied &&
      orphan_guard_recoveries->at(1) == orphan_guard_expected &&
      orphan_guard_recovery_content == "prior guarded video" &&
      orphan_guard_recovery_log_content == "prior guarded log" && prior_guard_recovery_content == "new stale video" &&
      prior_guard_recovery_log_content == "new stale log" && !fs::exists(orphan_guard_stale) &&
      !fs::exists(orphan_guard_stale_log) && !fs::exists(orphan_guard_occupied.string() + ".hstream-pin") &&
      !fs::exists(orphan_guard_occupied_log.string() + ".hstream-pin");
  if (!orphan_guard_recovery_ok) {
    std::cerr << "orphan guard recovery status=" << orphan_guard_recoveries.status()
              << " size=" << (orphan_guard_recoveries.ok() ? orphan_guard_recoveries->size() : 0)
              << " new-video=" << orphan_guard_recovery_content << " new-log=" << orphan_guard_recovery_log_content
              << " prior-video=" << prior_guard_recovery_content << " prior-log=" << prior_guard_recovery_log_content
              << '\n';
  }
  ok &= expect(
      orphan_guard_recovery_ok,
      "Recovery must reconcile an older guard-only transaction before preserving a new stale archive");

  const auto interrupted_recovery_is_reconciled =
      [&](const std::string& name, bool has_log, bool destination_log_exists, bool destination_video_exists) {
        const fs::path interrupted_dir = root / ("archive-interrupted-" + name);
        fs::create_directories(interrupted_dir);
        const fs::path configured = interrupted_dir / (name + ".mkv");
        const fs::path source = interrupted_dir / (name + ".hstream-run-99999999-dead.mkv");
        const fs::path source_log = source.string() + ".log";
        const fs::path destination = interrupted_dir / (name + "-finalization-failed.mkv");
        const fs::path destination_log = destination.string() + ".log";
        std::ofstream(source, std::ios::binary) << name << " video";
        if (has_log)
          std::ofstream(source_log, std::ios::binary) << name << " log";
        if (destination_log_exists)
          fs::create_hard_link(has_log ? source_log : source, destination_log);
        if (destination_video_exists)
          fs::create_hard_link(source, destination);

        const auto recoveries = hm::configurator_internal::recover_stale_archive_work_files(configured);
        std::ifstream recovered_video_stream(destination, std::ios::binary);
        const std::string recovered_video{
            std::istreambuf_iterator<char>(recovered_video_stream), std::istreambuf_iterator<char>()};
        std::ifstream recovered_log_stream(destination_log, std::ios::binary);
        const std::string recovered_log{
            std::istreambuf_iterator<char>(recovered_log_stream), std::istreambuf_iterator<char>()};
        return recoveries.ok() && recoveries->size() == 1 && recoveries->front() == destination &&
            recovered_video == name + " video" && !fs::exists(source) && !fs::exists(source_log) &&
            (has_log ? recovered_log == name + " log" : !fs::exists(destination_log));
      };
  ok &= expect(
      interrupted_recovery_is_reconciled("sidecar-log-linked", true, true, false) &&
          interrupted_recovery_is_reconciled("sidecar-pair-linked", true, true, true) &&
          interrupted_recovery_is_reconciled("no-log-marker-linked", false, true, false) &&
          interrupted_recovery_is_reconciled("no-log-pair-linked", false, true, true) &&
          interrupted_recovery_is_reconciled("no-log-video-linked", false, false, true),
      "Restart recovery must reconcile every interrupted hard-link publication boundary without splitting artifacts");

  const fs::path guarded_reconcile_dir = root / "archive-guarded-reconcile";
  fs::create_directories(guarded_reconcile_dir);
  const fs::path guarded_reconcile_configured = guarded_reconcile_dir / "guarded.mkv";
  const fs::path guarded_reconcile_recovery = guarded_reconcile_dir / "guarded-finalization-failed.mkv";
  const fs::path guarded_reconcile_log = guarded_reconcile_recovery.string() + ".log";
  const fs::path guarded_reconcile_expected = guarded_reconcile_dir / "guarded-finalization-failed-1.mkv";
  const fs::path guarded_reconcile_expected_log = guarded_reconcile_expected.string() + ".log";
  const fs::path guarded_video_backing = guarded_reconcile_dir / "trusted-video-backing";
  const fs::path guarded_log_backing = guarded_reconcile_dir / "trusted-log-backing";
  std::ofstream(guarded_video_backing, std::ios::binary) << "trusted guarded video";
  std::ofstream(guarded_log_backing, std::ios::binary) << "trusted guarded log";
  std::ofstream(guarded_reconcile_recovery, std::ios::binary) << "foreign visible recovery";
  fs::create_hard_link(guarded_log_backing, guarded_reconcile_log);
  fs::create_hard_link(guarded_video_backing, guarded_reconcile_recovery.string() + ".hstream-pin");
  fs::create_hard_link(guarded_log_backing, guarded_reconcile_log.string() + ".hstream-pin");
  const auto guarded_reconciled =
      hm::configurator_internal::recover_stale_archive_work_files(guarded_reconcile_configured);
  std::ifstream guarded_reconciled_video_stream(guarded_reconcile_expected, std::ios::binary);
  const std::string guarded_reconciled_video{
      std::istreambuf_iterator<char>(guarded_reconciled_video_stream), std::istreambuf_iterator<char>()};
  std::ifstream guarded_reconciled_log_stream(guarded_reconcile_expected_log, std::ios::binary);
  const std::string guarded_reconciled_log{
      std::istreambuf_iterator<char>(guarded_reconciled_log_stream), std::istreambuf_iterator<char>()};
  std::ifstream guarded_foreign_stream(guarded_reconcile_recovery, std::ios::binary);
  const std::string guarded_foreign{
      std::istreambuf_iterator<char>(guarded_foreign_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      guarded_reconciled.ok() && guarded_reconciled->size() == 1 &&
          guarded_reconciled->front() == guarded_reconcile_expected &&
          guarded_reconciled_video == "trusted guarded video" && guarded_reconciled_log == "trusted guarded log" &&
          guarded_foreign == "foreign visible recovery" &&
          !fs::exists(guarded_reconcile_expected.string() + ".hstream-pin") &&
          !fs::exists(guarded_reconcile_expected_log.string() + ".hstream-pin"),
      "Restart reconciliation must rescue trusted guarded identities instead of reporting a replaced visible pair");

  const fs::path unguarded_reconcile_dir = root / "archive-unguarded-reconcile";
  fs::create_directories(unguarded_reconcile_dir);
  const fs::path unguarded_reconcile_configured = unguarded_reconcile_dir / "unguarded.mkv";
  const fs::path unguarded_reconcile_recovery = unguarded_reconcile_dir / "unguarded-finalization-failed.mkv";
  std::ofstream(unguarded_reconcile_recovery, std::ios::binary) << "unguarded foreign recovery";
  const auto unguarded_reconciled =
      hm::configurator_internal::recover_stale_archive_work_files(unguarded_reconcile_configured);
  ok &= expect(
      unguarded_reconciled.ok() && unguarded_reconciled->empty() && fs::is_regular_file(unguarded_reconcile_recovery),
      "Restart reconciliation must not report an unguarded recovery artifact as an interrupted committed transaction");

  const auto interrupted_collision_is_reconciled =
      [&](const std::string& name, bool has_log, bool destination_video_is_foreign) {
        const fs::path interrupted_dir = root / ("archive-interrupted-collision-" + name);
        fs::create_directories(interrupted_dir);
        const fs::path configured = interrupted_dir / (name + ".mkv");
        const fs::path source = interrupted_dir / (name + ".hstream-run-99999999-dead.mkv");
        const fs::path source_log = source.string() + ".log";
        const fs::path collided_destination = interrupted_dir / (name + "-finalization-failed.mkv");
        const fs::path collided_log = collided_destination.string() + ".log";
        const fs::path expected_destination = interrupted_dir / (name + "-finalization-failed-1.mkv");
        const fs::path expected_log = expected_destination.string() + ".log";
        std::ofstream(source, std::ios::binary) << name << " video";
        if (has_log)
          std::ofstream(source_log, std::ios::binary) << name << " log";
        if (destination_video_is_foreign) {
          std::ofstream(collided_destination, std::ios::binary) << "foreign video";
          fs::create_hard_link(has_log ? source_log : source, collided_log);
        } else {
          fs::create_hard_link(source, collided_destination);
          std::ofstream(collided_log, std::ios::binary) << "foreign log";
        }

        const auto recoveries = hm::configurator_internal::recover_stale_archive_work_files(configured);
        std::ifstream collided_video_stream(collided_destination, std::ios::binary);
        const std::string collided_video{
            std::istreambuf_iterator<char>(collided_video_stream), std::istreambuf_iterator<char>()};
        std::ifstream collided_log_stream(collided_log, std::ios::binary);
        const std::string collided_log_content{
            std::istreambuf_iterator<char>(collided_log_stream), std::istreambuf_iterator<char>()};
        std::ifstream recovered_video_stream(expected_destination, std::ios::binary);
        const std::string recovered_video{
            std::istreambuf_iterator<char>(recovered_video_stream), std::istreambuf_iterator<char>()};
        std::ifstream recovered_log_stream(expected_log, std::ios::binary);
        const std::string recovered_log{
            std::istreambuf_iterator<char>(recovered_log_stream), std::istreambuf_iterator<char>()};
        const bool collided_pair_cleaned = destination_video_is_foreign
            ? collided_video == "foreign video" && !fs::exists(collided_log)
            : !fs::exists(collided_destination) && collided_log_content == "foreign log";
        return recoveries.ok() && recoveries->size() == 1 && recoveries->front() == expected_destination &&
            collided_pair_cleaned && !fs::exists(source) && !fs::exists(source_log) &&
            recovered_video == name + " video" &&
            (has_log ? recovered_log == name + " log" : !fs::exists(expected_log));
      };
  ok &= expect(
      interrupted_collision_is_reconciled("log-with-foreign-video", true, true) &&
          interrupted_collision_is_reconciled("video-with-foreign-log", true, false) &&
          interrupted_collision_is_reconciled("marker-with-foreign-video", false, true) &&
          interrupted_collision_is_reconciled("video-with-foreign-sidecar", false, false),
      "Restart recovery must remove only its owned half of an interrupted collision before retrying another suffix");

  const fs::path replaced_publication_dir = root / "archive-replaced-publication";
  fs::create_directories(replaced_publication_dir);
  const fs::path replaced_publication_source = replaced_publication_dir / "replaced.mkv";
  const fs::path replaced_publication_source_log = replaced_publication_source.string() + ".log";
  const fs::path replaced_publication_destination = replaced_publication_dir / "replaced-finalization-failed.mkv";
  const fs::path replaced_publication_destination_log = replaced_publication_destination.string() + ".log";
  std::ofstream(replaced_publication_source, std::ios::binary) << "trusted source video";
  std::ofstream(replaced_publication_source_log, std::ios::binary) << "trusted source log";
  g_setenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_RECOVERY", "1", TRUE);
  const auto replaced_publication =
      hm::configurator_internal::preserve_existing_archive_work_file(replaced_publication_source);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_RECOVERY");
  std::ifstream replaced_publication_stream(replaced_publication_destination, std::ios::binary);
  const std::string replaced_publication_content{
      std::istreambuf_iterator<char>(replaced_publication_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      !replaced_publication.ok() && fs::is_regular_file(replaced_publication_source) &&
          fs::is_regular_file(replaced_publication_source_log) &&
          replaced_publication_content == "injected foreign archive recovery" &&
          !fs::exists(replaced_publication_destination_log),
      "A replaced published destination must retain both trusted sources and clean only the owned partial sidecar");

  const fs::path replaced_marker_dir = root / "archive-replaced-marker";
  fs::create_directories(replaced_marker_dir);
  const fs::path replaced_marker_source = replaced_marker_dir / "marker.mkv";
  const fs::path replaced_marker_destination = replaced_marker_dir / "marker-finalization-failed.mkv";
  const fs::path replaced_marker_sidecar = replaced_marker_destination.string() + ".log";
  std::ofstream(replaced_marker_source, std::ios::binary) << "trusted video without a log";
  g_setenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_RECOVERY_MARKER", "1", TRUE);
  const auto replaced_marker = hm::configurator_internal::preserve_existing_archive_work_file(replaced_marker_source);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_RECOVERY_MARKER");
  std::ifstream replaced_marker_source_stream(replaced_marker_source, std::ios::binary);
  const std::string replaced_marker_source_content{
      std::istreambuf_iterator<char>(replaced_marker_source_stream), std::istreambuf_iterator<char>()};
  std::ifstream replaced_marker_stream(replaced_marker_sidecar, std::ios::binary);
  const std::string replaced_marker_content{
      std::istreambuf_iterator<char>(replaced_marker_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      !replaced_marker.ok() && replaced_marker_source_content == "trusted video without a log" &&
          !fs::exists(replaced_marker_destination) &&
          replaced_marker_content == "injected foreign archive recovery marker",
      "No-log recovery must keep its reservation through video publication and retain the source if that marker is replaced");

  const fs::path post_quarantine_dir = root / "archive-post-quarantine-replacement";
  fs::create_directories(post_quarantine_dir);
  const fs::path post_quarantine_source = post_quarantine_dir / "post-quarantine.mkv";
  const fs::path post_quarantine_log = post_quarantine_source.string() + ".log";
  const fs::path post_quarantine_destination = post_quarantine_dir / "post-quarantine-finalization-failed.mkv";
  const fs::path post_quarantine_destination_log = post_quarantine_destination.string() + ".log";
  std::ofstream(post_quarantine_source, std::ios::binary) << "trusted post-quarantine video";
  std::ofstream(post_quarantine_log, std::ios::binary) << "trusted post-quarantine log";
  g_setenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_AFTER_QUARANTINE", "1", TRUE);
  const auto post_quarantine = hm::configurator_internal::preserve_existing_archive_work_file(post_quarantine_source);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_AFTER_QUARANTINE");
  std::ifstream post_quarantine_source_stream(post_quarantine_source, std::ios::binary);
  const std::string post_quarantine_source_content{
      std::istreambuf_iterator<char>(post_quarantine_source_stream), std::istreambuf_iterator<char>()};
  std::ifstream post_quarantine_destination_stream(post_quarantine_destination, std::ios::binary);
  const std::string post_quarantine_destination_content{
      std::istreambuf_iterator<char>(post_quarantine_destination_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      !post_quarantine.ok() && post_quarantine_source_content == "trusted post-quarantine video" &&
          fs::is_regular_file(post_quarantine_log) &&
          post_quarantine_destination_content == "injected foreign archive after quarantine" &&
          !fs::exists(post_quarantine_destination_log),
      "Protected cleanup must restore its pinned source and retire its partial sidecar if publication is replaced after quarantine unlink");

  const fs::path early_quarantine_dir = root / "archive-early-quarantine-rollback";
  fs::create_directories(early_quarantine_dir);
  const fs::path early_quarantine_source = early_quarantine_dir / "early-quarantine.mkv";
  const fs::path early_quarantine_recovery = early_quarantine_dir / "early-quarantine-finalization-failed.mkv";
  const fs::path early_quarantine_guard = early_quarantine_source.string() + ".hstream-pin";
  std::ofstream(early_quarantine_source, std::ios::binary) << "trusted early-quarantine video";
  g_setenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_PUBLICATION_AND_SOURCE_DURING_QUARANTINE", "1", TRUE);
  const auto early_quarantine = hm::configurator_internal::preserve_existing_archive_work_file(early_quarantine_source);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_PUBLICATION_AND_SOURCE_DURING_QUARANTINE");
  std::ifstream early_quarantine_source_stream(early_quarantine_source, std::ios::binary);
  const std::string early_quarantine_source_content{
      std::istreambuf_iterator<char>(early_quarantine_source_stream), std::istreambuf_iterator<char>()};
  std::ifstream early_quarantine_recovery_stream(early_quarantine_recovery, std::ios::binary);
  const std::string early_quarantine_recovery_content{
      std::istreambuf_iterator<char>(early_quarantine_recovery_stream), std::istreambuf_iterator<char>()};
  std::ifstream early_quarantine_guard_stream(early_quarantine_guard, std::ios::binary);
  const std::string early_quarantine_guard_content{
      std::istreambuf_iterator<char>(early_quarantine_guard_stream), std::istreambuf_iterator<char>()};
  bool early_quarantine_hidden_entry = false;
  for (const auto& entry : fs::directory_iterator(early_quarantine_dir)) {
    if (entry.is_directory() && entry.path().filename().string().find(".hstream-cleanup-") == 0)
      early_quarantine_hidden_entry = true;
  }
  ok &= expect(
      !early_quarantine.ok() && early_quarantine_source_content == "injected foreign source during quarantine" &&
          early_quarantine_recovery_content == "injected foreign publication during quarantine" &&
          early_quarantine_guard_content == "trusted early-quarantine video" && !early_quarantine_hidden_entry,
      "Early quarantine rollback must retain the pinned inode at a durable guard when both original and published names are replaced");

  const fs::path interrupted_quarantine_dir = root / "archive-interrupted-quarantine";
  fs::create_directories(interrupted_quarantine_dir);
  const fs::path interrupted_quarantine_configured = interrupted_quarantine_dir / "interrupted-quarantine.mkv";
  const fs::path interrupted_quarantine_source =
      interrupted_quarantine_dir / "interrupted-quarantine.hstream-run-99999999-dead.mkv";
  const fs::path interrupted_quarantine_source_log = interrupted_quarantine_source.string() + ".log";
  const fs::path interrupted_quarantine_recovery =
      interrupted_quarantine_dir / "interrupted-quarantine-finalization-failed.mkv";
  const fs::path unrelated_cleanup_lookalike = interrupted_quarantine_dir / "notes.hstream-cleanup-pin";
  const fs::path unrelated_cleanup_directory =
      interrupted_quarantine_dir / ".hstream-cleanup-v2-dddddddd-eeee-4fff-8aaa-bbbbbbbbbbbb";
  const fs::path unrelated_cleanup_guard = unrelated_cleanup_directory / "guard";
  const fs::path unrelated_cleanup_fallback = unrelated_cleanup_directory / "fallback";
  const fs::path unrelated_cleanup_sibling_owner = unrelated_cleanup_directory.string() + ".hstream-owner";
  std::ofstream(interrupted_quarantine_source, std::ios::binary) << "trusted interrupted-quarantine video";
  std::ofstream(interrupted_quarantine_source_log, std::ios::binary) << "trusted interrupted-quarantine log";
  std::ofstream(unrelated_cleanup_lookalike, std::ios::binary) << "unrelated cleanup-looking notes";
  fs::create_directories(unrelated_cleanup_directory);
  std::ofstream(unrelated_cleanup_guard, std::ios::binary) << "unrelated exact cleanup guard";
  fs::create_hard_link(unrelated_cleanup_guard, unrelated_cleanup_fallback);
  std::ofstream(unrelated_cleanup_sibling_owner, std::ios::binary) << "hstream-cleanup-v2\ndW5yZWxhdGVkLm1rdg==";
  g_setenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_QUARANTINE", "1", TRUE);
  const auto interrupted_quarantine_first =
      hm::configurator_internal::recover_stale_archive_work_files(interrupted_quarantine_configured);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_QUARANTINE");
  const auto interrupted_quarantine_restart =
      hm::configurator_internal::recover_stale_archive_work_files(interrupted_quarantine_configured);
  std::ifstream interrupted_quarantine_video_stream(interrupted_quarantine_recovery, std::ios::binary);
  const std::string interrupted_quarantine_video{
      std::istreambuf_iterator<char>(interrupted_quarantine_video_stream), std::istreambuf_iterator<char>()};
  std::ifstream interrupted_quarantine_log_stream(interrupted_quarantine_recovery.string() + ".log", std::ios::binary);
  const std::string interrupted_quarantine_log{
      std::istreambuf_iterator<char>(interrupted_quarantine_log_stream), std::istreambuf_iterator<char>()};
  std::ifstream unrelated_cleanup_lookalike_stream(unrelated_cleanup_lookalike, std::ios::binary);
  const std::string unrelated_cleanup_lookalike_content{
      std::istreambuf_iterator<char>(unrelated_cleanup_lookalike_stream), std::istreambuf_iterator<char>()};
  bool interrupted_quarantine_cleanup_remains = false;
  for (const auto& entry : fs::directory_iterator(interrupted_quarantine_dir)) {
    if (entry.is_directory() && entry.path() != unrelated_cleanup_directory &&
        entry.path().filename().string().find(".hstream-cleanup-") == 0)
      interrupted_quarantine_cleanup_remains = true;
  }
  ok &= expect(
      !interrupted_quarantine_first.ok() && interrupted_quarantine_restart.ok() &&
          interrupted_quarantine_restart->size() == 1 &&
          interrupted_quarantine_restart->front() == interrupted_quarantine_recovery &&
          interrupted_quarantine_video == "trusted interrupted-quarantine video" &&
          interrupted_quarantine_log == "trusted interrupted-quarantine log" &&
          !interrupted_quarantine_cleanup_remains &&
          !fs::exists(interrupted_quarantine_source.string() + ".hstream-cleanup-pin") &&
          unrelated_cleanup_lookalike_content == "unrelated cleanup-looking notes" &&
          fs::is_directory(unrelated_cleanup_directory) && fs::exists(unrelated_cleanup_guard) &&
          fs::exists(unrelated_cleanup_fallback) && fs::exists(unrelated_cleanup_sibling_owner) &&
          !fs::exists(interrupted_quarantine_dir / "notes"),
      "Restart must reconcile a crash after quarantine without claiming unrelated cleanup-looking user entries");

  const fs::path live_cleanup_dir = root / "archive-live-cleanup-lock";
  fs::create_directories(live_cleanup_dir);
  const fs::path live_cleanup_configured = live_cleanup_dir / "live-cleanup.mkv";
  const fs::path live_cleanup_source = live_cleanup_dir / "live-cleanup.hstream-run-99999999-dead.mkv";
  const fs::path live_cleanup_fallback = live_cleanup_source.string() + ".hstream-cleanup-pin";
  const fs::path live_cleanup_transaction =
      live_cleanup_dir / ".hstream-cleanup-v2-aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
  const fs::path live_cleanup_owner = live_cleanup_transaction / "owner";
  std::ofstream(live_cleanup_source, std::ios::binary) << "trusted live cleanup video";
  fs::create_hard_link(live_cleanup_source, live_cleanup_fallback);
  fs::create_directories(live_cleanup_transaction);
  fs::rename(live_cleanup_source, live_cleanup_transaction / "entry");
  const std::string live_cleanup_target_name = live_cleanup_source.filename().string();
  gchar* live_cleanup_encoded_target = g_base64_encode(
      reinterpret_cast<const guchar*>(live_cleanup_target_name.data()), live_cleanup_target_name.size());
  std::ofstream(live_cleanup_owner, std::ios::binary) << "hstream-cleanup-v2\n" << live_cleanup_encoded_target;
  g_free(live_cleanup_encoded_target);
  const int live_cleanup_fd = ::open(live_cleanup_transaction.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  const bool live_cleanup_locked = live_cleanup_fd >= 0 && ::flock(live_cleanup_fd, LOCK_EX | LOCK_NB) == 0;
  const auto live_cleanup_skipped =
      hm::configurator_internal::recover_stale_archive_work_files(live_cleanup_configured);
  const bool live_cleanup_unchanged = live_cleanup_skipped.ok() && live_cleanup_skipped->empty() &&
      fs::exists(live_cleanup_transaction / "entry") && fs::exists(live_cleanup_fallback) &&
      fs::exists(live_cleanup_owner);
  if (live_cleanup_fd >= 0)
    ::close(live_cleanup_fd);
  const auto live_cleanup_resumed =
      hm::configurator_internal::recover_stale_archive_work_files(live_cleanup_configured);
  std::ifstream live_cleanup_recovery_stream(
      live_cleanup_dir / "live-cleanup-finalization-failed.mkv", std::ios::binary);
  const std::string live_cleanup_recovery_content{
      std::istreambuf_iterator<char>(live_cleanup_recovery_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      live_cleanup_locked && live_cleanup_unchanged && live_cleanup_resumed.ok() && live_cleanup_resumed->size() == 1 &&
          live_cleanup_recovery_content == "trusted live cleanup video" && !fs::exists(live_cleanup_transaction) &&
          !fs::exists(live_cleanup_owner) && !fs::exists(live_cleanup_fallback),
      "Cleanup reconciliation must skip a live locked transaction and resume it only after ownership is released");

  const fs::path scoped_cleanup_dir = root / "archive-scoped-cleanup-owner";
  fs::create_directories(scoped_cleanup_dir);
  const fs::path scoped_cleanup_configured = scoped_cleanup_dir / "scoped-cleanup.mkv";
  const fs::path scoped_cleanup_target = scoped_cleanup_dir / "scoped-cleanup.hstream-run-99999999-dead.mkv";
  const fs::path scoped_cleanup_unrelated = scoped_cleanup_dir / "unrelated-hardlink.mkv";
  const fs::path scoped_cleanup_transaction =
      scoped_cleanup_dir / ".hstream-cleanup-v2-bbbbbbbb-cccc-4ddd-8eee-ffffffffffff";
  fs::create_directories(scoped_cleanup_transaction);
  const fs::path scoped_cleanup_entry = scoped_cleanup_transaction / "entry";
  const fs::path scoped_cleanup_owner = scoped_cleanup_transaction / "owner";
  std::ofstream(scoped_cleanup_entry, std::ios::binary) << "trusted scoped cleanup inode";
  fs::create_hard_link(scoped_cleanup_entry, scoped_cleanup_unrelated);
  const std::string scoped_cleanup_target_name = scoped_cleanup_target.filename().string();
  gchar* scoped_cleanup_encoded_target = g_base64_encode(
      reinterpret_cast<const guchar*>(scoped_cleanup_target_name.data()), scoped_cleanup_target_name.size());
  std::ofstream(scoped_cleanup_owner, std::ios::binary) << "hstream-cleanup-v2\n" << scoped_cleanup_encoded_target;
  g_free(scoped_cleanup_encoded_target);
  const auto scoped_cleanup_recovery =
      hm::configurator_internal::recover_stale_archive_work_files(scoped_cleanup_configured);
  std::ifstream scoped_cleanup_unrelated_stream(scoped_cleanup_unrelated, std::ios::binary);
  const std::string scoped_cleanup_unrelated_content{
      std::istreambuf_iterator<char>(scoped_cleanup_unrelated_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      !scoped_cleanup_recovery.ok() && !fs::exists(scoped_cleanup_target) &&
          !fs::exists(scoped_cleanup_target.string() + ".hstream-cleanup-pin") && fs::exists(scoped_cleanup_entry) &&
          fs::exists(scoped_cleanup_owner) && scoped_cleanup_unrelated_content == "trusted scoped cleanup inode",
      "Cleanup reconciliation must retain private evidence instead of claiming a hardlink outside its owner target");

  const fs::path fallback_retirement_dir = root / "archive-fallback-retirement-interruption";
  fs::create_directories(fallback_retirement_dir);
  const fs::path fallback_retirement_configured = fallback_retirement_dir / "fallback-retirement.mkv";
  const fs::path fallback_retirement_source =
      fallback_retirement_dir / "fallback-retirement.hstream-run-99999999-dead.mkv";
  const fs::path fallback_retirement_recovery = fallback_retirement_dir / "fallback-retirement-finalization-failed.mkv";
  std::ofstream(fallback_retirement_source, std::ios::binary) << "trusted fallback-retirement video";
  g_setenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_FALLBACK_QUARANTINE", "1", TRUE);
  const auto fallback_retirement_first =
      hm::configurator_internal::recover_stale_archive_work_files(fallback_retirement_configured);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_FALLBACK_QUARANTINE");
  const auto fallback_retirement_restart =
      hm::configurator_internal::recover_stale_archive_work_files(fallback_retirement_configured);
  std::ifstream fallback_retirement_recovery_stream(fallback_retirement_recovery, std::ios::binary);
  const std::string fallback_retirement_recovery_content{
      std::istreambuf_iterator<char>(fallback_retirement_recovery_stream), std::istreambuf_iterator<char>()};
  bool fallback_retirement_cleanup_remains = false;
  for (const fs::path& entry : fs::directory_iterator(fallback_retirement_dir)) {
    if (entry.filename().string().rfind(".hstream-cleanup-", 0) == 0)
      fallback_retirement_cleanup_remains = true;
  }
  ok &= expect(
      !fallback_retirement_first.ok() && fallback_retirement_restart.ok() && fallback_retirement_restart->size() == 1 &&
          fallback_retirement_recovery_content == "trusted fallback-retirement video" &&
          !fallback_retirement_cleanup_remains,
      "Restart must finish a committed deletion interrupted after cleanup fallback quarantine");

  const fs::path committed_retirement_dir = root / "archive-committed-retirement-interruption";
  fs::create_directories(committed_retirement_dir);
  const fs::path committed_retirement_target = committed_retirement_dir / "committed-retirement.mkv";
  std::ofstream(committed_retirement_target, std::ios::binary) << "trusted committed-retirement video";
  struct stat committed_retirement_stat{};
  const bool committed_retirement_stat_ok =
      ::lstat(committed_retirement_target.c_str(), &committed_retirement_stat) == 0;
  g_setenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_FALLBACK_RETIREMENT", "1", TRUE);
  const absl::Status committed_retirement_first = committed_retirement_stat_ok
      ? hm::configurator_internal::remove_archive_entry_if_owned_for_test(
            committed_retirement_target,
            static_cast<uintmax_t>(committed_retirement_stat.st_dev),
            static_cast<uintmax_t>(committed_retirement_stat.st_ino))
      : absl::InternalError("committed-retirement fixture identity is unavailable");
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_FALLBACK_RETIREMENT");
  fs::path committed_retirement_transaction;
  for (const fs::path& entry : fs::directory_iterator(committed_retirement_dir)) {
    if (fs::is_directory(entry) && entry.filename().string().rfind(".hstream-cleanup-v2-", 0) == 0)
      committed_retirement_transaction = entry;
  }
  const bool committed_retirement_authenticated = !committed_retirement_transaction.empty() &&
      fs::exists(committed_retirement_transaction / "owner") &&
      fs::exists(committed_retirement_transaction / "committed") &&
      fs::exists(committed_retirement_transaction / "guard") &&
      !fs::exists(committed_retirement_transaction / "fallback") &&
      !fs::exists(committed_retirement_transaction / "entry");
  const auto committed_retirement_restart =
      hm::configurator_internal::recover_stale_archive_work_files(committed_retirement_dir / "configured.mkv");
  ok &= expect(
      !committed_retirement_first.ok() && committed_retirement_authenticated && committed_retirement_restart.ok() &&
          committed_retirement_restart->empty() && !fs::exists(committed_retirement_target) &&
          !fs::exists(committed_retirement_transaction),
      "A durable commit record must finish deletion after interruption between fallback and guard retirement");

  const fs::path pending_commit_dir = root / "archive-pending-commit-publication";
  fs::create_directories(pending_commit_dir);
  const fs::path pending_commit_target = pending_commit_dir / "pending-commit.mkv";
  std::ofstream(pending_commit_target, std::ios::binary) << "trusted pending-commit video";
  struct stat pending_commit_stat{};
  const bool pending_commit_stat_ok = ::lstat(pending_commit_target.c_str(), &pending_commit_stat) == 0;
  g_setenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_BEFORE_CLEANUP_COMMIT_PUBLISH", "1", TRUE);
  const absl::Status pending_commit_first = pending_commit_stat_ok
      ? hm::configurator_internal::remove_archive_entry_if_owned_for_test(
            pending_commit_target,
            static_cast<uintmax_t>(pending_commit_stat.st_dev),
            static_cast<uintmax_t>(pending_commit_stat.st_ino))
      : absl::InternalError("pending-commit fixture identity is unavailable");
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_BEFORE_CLEANUP_COMMIT_PUBLISH");
  fs::path pending_commit_transaction;
  for (const fs::path& entry : fs::directory_iterator(pending_commit_dir)) {
    if (fs::is_directory(entry) && entry.filename().string().rfind(".hstream-cleanup-v2-", 0) == 0)
      pending_commit_transaction = entry;
  }
  const bool pending_commit_unpublished = !pending_commit_transaction.empty() &&
      fs::exists(pending_commit_transaction / "owner") &&
      fs::exists(pending_commit_transaction / "committed.pending") &&
      !fs::exists(pending_commit_transaction / "committed") &&
      fs::exists(pending_commit_transaction / "guard") && fs::exists(pending_commit_transaction / "fallback") &&
      !fs::exists(pending_commit_target);
  const auto pending_commit_restart =
      hm::configurator_internal::recover_stale_archive_work_files(pending_commit_dir / "configured.mkv");
  std::ifstream pending_commit_restored_stream(pending_commit_target, std::ios::binary);
  const std::string pending_commit_restored{
      std::istreambuf_iterator<char>(pending_commit_restored_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      !pending_commit_first.ok() && pending_commit_unpublished && pending_commit_restart.ok() &&
          pending_commit_restart->empty() && pending_commit_restored == "trusted pending-commit video" &&
          !fs::exists(pending_commit_transaction),
      "An interruption before atomic commit publication must leave no visible commit and roll deletion back on restart");

  const fs::path foreign_fallback_dir = root / "archive-foreign-private-fallback";
  fs::create_directories(foreign_fallback_dir);
  const fs::path foreign_fallback_target = foreign_fallback_dir / "foreign-fallback-target.mkv";
  const fs::path foreign_fallback_transaction =
      foreign_fallback_dir / ".hstream-cleanup-v2-12345678-90ab-4cde-8fab-1234567890ab";
  const fs::path foreign_fallback_guard = foreign_fallback_transaction / "guard";
  const fs::path foreign_fallback_private = foreign_fallback_transaction / "fallback";
  const fs::path foreign_fallback_owner = foreign_fallback_transaction / "owner";
  fs::create_directories(foreign_fallback_transaction);
  std::ofstream(foreign_fallback_guard, std::ios::binary) << "trusted private cleanup guard";
  std::ofstream(foreign_fallback_private, std::ios::binary) << "foreign private cleanup fallback";
  const std::string foreign_fallback_target_name = foreign_fallback_target.filename().string();
  gchar* foreign_fallback_encoded_target = g_base64_encode(
      reinterpret_cast<const guchar*>(foreign_fallback_target_name.data()), foreign_fallback_target_name.size());
  std::ofstream(foreign_fallback_owner, std::ios::binary) << "hstream-cleanup-v2\n" << foreign_fallback_encoded_target;
  g_free(foreign_fallback_encoded_target);
  const auto foreign_fallback_recovery =
      hm::configurator_internal::recover_stale_archive_work_files(foreign_fallback_dir / "configured.mkv");
  std::ifstream foreign_fallback_guard_stream(foreign_fallback_guard, std::ios::binary);
  const std::string foreign_fallback_guard_content{
      std::istreambuf_iterator<char>(foreign_fallback_guard_stream), std::istreambuf_iterator<char>()};
  std::ifstream foreign_fallback_private_stream(foreign_fallback_private, std::ios::binary);
  const std::string foreign_fallback_private_content{
      std::istreambuf_iterator<char>(foreign_fallback_private_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      !foreign_fallback_recovery.ok() && !fs::exists(foreign_fallback_target) && fs::exists(foreign_fallback_owner) &&
          foreign_fallback_guard_content == "trusted private cleanup guard" &&
          foreign_fallback_private_content == "foreign private cleanup fallback",
      "An uncommitted foreign private fallback must never authorize deletion of the trusted guard");

  const fs::path failed_private_unlink_dir = root / "archive-failed-private-unlink";
  fs::create_directories(failed_private_unlink_dir);
  const fs::path failed_private_unlink_target = failed_private_unlink_dir / "failed-private-unlink.mkv";
  std::ofstream(failed_private_unlink_target, std::ios::binary) << "trusted failed-private-unlink video";
  struct stat failed_private_unlink_stat{};
  const bool failed_private_unlink_stat_ok =
      ::lstat(failed_private_unlink_target.c_str(), &failed_private_unlink_stat) == 0;
  g_setenv(
      "HSTREAM_CONFIGURATOR_TEST_ARCHIVE_PRIVATE_ENTRY_UNLINK_FAILURE", failed_private_unlink_target.c_str(), TRUE);
  const absl::Status failed_private_unlink = failed_private_unlink_stat_ok
      ? hm::configurator_internal::remove_archive_entry_if_owned_for_test(
            failed_private_unlink_target,
            static_cast<uintmax_t>(failed_private_unlink_stat.st_dev),
            static_cast<uintmax_t>(failed_private_unlink_stat.st_ino))
      : absl::InternalError("failed-private-unlink fixture identity is unavailable");
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_ARCHIVE_PRIVATE_ENTRY_UNLINK_FAILURE");
  fs::path failed_private_unlink_transaction;
  for (const fs::path& entry : fs::directory_iterator(failed_private_unlink_dir)) {
    if (fs::is_directory(entry) && entry.filename().string().rfind(".hstream-cleanup-v2-", 0) == 0)
      failed_private_unlink_transaction = entry;
  }
  const bool failed_private_unlink_authenticated = !failed_private_unlink_transaction.empty() &&
      fs::exists(failed_private_unlink_transaction / "owner") &&
      fs::exists(failed_private_unlink_transaction / "entry") &&
      fs::exists(failed_private_unlink_transaction / "guard");
  const auto failed_private_unlink_restart =
      hm::configurator_internal::recover_stale_archive_work_files(failed_private_unlink_dir / "configured.mkv");
  std::ifstream failed_private_unlink_restored_stream(failed_private_unlink_target, std::ios::binary);
  const std::string failed_private_unlink_restored{
      std::istreambuf_iterator<char>(failed_private_unlink_restored_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      !failed_private_unlink.ok() && failed_private_unlink_authenticated && failed_private_unlink_restart.ok() &&
          failed_private_unlink_restart->empty() &&
          failed_private_unlink_restored == "trusted failed-private-unlink video" &&
          !fs::exists(failed_private_unlink_transaction),
      "A failed private unlink must retain authenticated transaction ownership until restart rollback succeeds");

  const fs::path concurrent_cleanup_dir = root / "archive-concurrent-cleanup";
  fs::create_directories(concurrent_cleanup_dir);
  const fs::path concurrent_cleanup_target = concurrent_cleanup_dir / "concurrent-cleanup.mkv";
  std::ofstream(concurrent_cleanup_target, std::ios::binary) << "trusted concurrent-cleanup video";
  struct stat concurrent_cleanup_stat{};
  const bool concurrent_cleanup_stat_ok = ::lstat(concurrent_cleanup_target.c_str(), &concurrent_cleanup_stat) == 0;
  std::atomic<bool> concurrent_cleanup_start{false};
  absl::Status concurrent_cleanup_first = absl::UnknownError("first concurrent remover did not run");
  absl::Status concurrent_cleanup_second = absl::UnknownError("second concurrent remover did not run");
  g_setenv("HSTREAM_CONFIGURATOR_TEST_DELAY_BEFORE_CLEANUP_SERIALIZATION", concurrent_cleanup_target.c_str(), TRUE);
  const auto concurrent_cleanup_remove = [&](absl::Status* result) {
    while (!concurrent_cleanup_start.load(std::memory_order_acquire))
      std::this_thread::yield();
    *result = hm::configurator_internal::remove_archive_entry_if_owned_for_test(
        concurrent_cleanup_target,
        static_cast<uintmax_t>(concurrent_cleanup_stat.st_dev),
        static_cast<uintmax_t>(concurrent_cleanup_stat.st_ino));
  };
  std::thread concurrent_cleanup_thread_one(concurrent_cleanup_remove, &concurrent_cleanup_first);
  std::thread concurrent_cleanup_thread_two(concurrent_cleanup_remove, &concurrent_cleanup_second);
  concurrent_cleanup_start.store(true, std::memory_order_release);
  concurrent_cleanup_thread_one.join();
  concurrent_cleanup_thread_two.join();
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_DELAY_BEFORE_CLEANUP_SERIALIZATION");
  bool concurrent_cleanup_artifacts_remain = false;
  for (const fs::path& entry : fs::directory_iterator(concurrent_cleanup_dir)) {
    const std::string name = entry.filename().string();
    concurrent_cleanup_artifacts_remain |=
        name.rfind(".hstream-cleanup-v2-", 0) == 0 || absl::EndsWith(name, ".hstream-cleanup-pin");
  }
  ok &= expect(
      concurrent_cleanup_stat_ok && concurrent_cleanup_first.ok() && concurrent_cleanup_second.ok() &&
          !fs::exists(concurrent_cleanup_target) && !concurrent_cleanup_artifacts_remain,
      "Concurrent removers must serialize before sharing or retiring a deterministic cleanup fallback");

  const fs::path interrupted_concurrent_dir = root / "archive-interrupted-concurrent-cleanup";
  fs::create_directories(interrupted_concurrent_dir);
  const fs::path interrupted_concurrent_target = interrupted_concurrent_dir / "interrupted-concurrent-cleanup.mkv";
  std::ofstream(interrupted_concurrent_target, std::ios::binary) << "trusted interrupted concurrent-cleanup video";
  struct stat interrupted_concurrent_stat{};
  const bool interrupted_concurrent_stat_ok =
      ::lstat(interrupted_concurrent_target.c_str(), &interrupted_concurrent_stat) == 0;
  std::atomic<bool> interrupted_concurrent_start{false};
  absl::Status interrupted_concurrent_first = absl::OkStatus();
  absl::Status interrupted_concurrent_second = absl::OkStatus();
  g_setenv("HSTREAM_CONFIGURATOR_TEST_DELAY_BEFORE_CLEANUP_SERIALIZATION", interrupted_concurrent_target.c_str(), TRUE);
  g_setenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_QUARANTINE", interrupted_concurrent_target.c_str(), TRUE);
  const auto interrupted_concurrent_remove = [&](absl::Status* result) {
    while (!interrupted_concurrent_start.load(std::memory_order_acquire))
      std::this_thread::yield();
    *result = hm::configurator_internal::remove_archive_entry_if_owned_for_test(
        interrupted_concurrent_target,
        static_cast<uintmax_t>(interrupted_concurrent_stat.st_dev),
        static_cast<uintmax_t>(interrupted_concurrent_stat.st_ino));
  };
  std::thread interrupted_concurrent_thread_one(interrupted_concurrent_remove, &interrupted_concurrent_first);
  std::thread interrupted_concurrent_thread_two(interrupted_concurrent_remove, &interrupted_concurrent_second);
  interrupted_concurrent_start.store(true, std::memory_order_release);
  interrupted_concurrent_thread_one.join();
  interrupted_concurrent_thread_two.join();
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_DELAY_BEFORE_CLEANUP_SERIALIZATION");
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_QUARANTINE");
  fs::path interrupted_concurrent_transaction;
  for (const fs::path& entry : fs::directory_iterator(interrupted_concurrent_dir)) {
    if (fs::is_directory(entry) && entry.filename().string().rfind(".hstream-cleanup-v2-", 0) == 0)
      interrupted_concurrent_transaction = entry;
  }
  const auto interrupted_concurrent_restart =
      hm::configurator_internal::recover_stale_archive_work_files(interrupted_concurrent_dir / "configured.mkv");
  std::ifstream interrupted_concurrent_restored_stream(interrupted_concurrent_target, std::ios::binary);
  const std::string interrupted_concurrent_restored{
      std::istreambuf_iterator<char>(interrupted_concurrent_restored_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      interrupted_concurrent_stat_ok && !interrupted_concurrent_first.ok() && !interrupted_concurrent_second.ok() &&
          !interrupted_concurrent_transaction.empty() && interrupted_concurrent_restart.ok() &&
          interrupted_concurrent_restart->empty() &&
          interrupted_concurrent_restored == "trusted interrupted concurrent-cleanup video" &&
          !fs::exists(interrupted_concurrent_transaction),
      "A second remover must not report success while an interrupted concurrent transaction can restore the target");

  const fs::path log_quarantine_dir = root / "archive-log-quarantine-interruption";
  fs::create_directories(log_quarantine_dir);
  const fs::path log_quarantine_configured = log_quarantine_dir / "log-quarantine.mkv";
  const fs::path log_quarantine_source = log_quarantine_dir / "log-quarantine.hstream-run-99999999-dead.mkv";
  const fs::path log_quarantine_source_log = log_quarantine_source.string() + ".log";
  const fs::path log_quarantine_recovery = log_quarantine_dir / "log-quarantine-finalization-failed.mkv";
  std::ofstream(log_quarantine_source, std::ios::binary) << "trusted log-quarantine video";
  std::ofstream(log_quarantine_source_log, std::ios::binary) << "trusted log-quarantine log";
  g_setenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_QUARANTINE", log_quarantine_source_log.c_str(), TRUE);
  const auto log_quarantine_first =
      hm::configurator_internal::recover_stale_archive_work_files(log_quarantine_configured);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_QUARANTINE");
  const auto log_quarantine_restart =
      hm::configurator_internal::recover_stale_archive_work_files(log_quarantine_configured);
  std::ifstream log_quarantine_recovery_stream(log_quarantine_recovery, std::ios::binary);
  const std::string log_quarantine_recovery_content{
      std::istreambuf_iterator<char>(log_quarantine_recovery_stream), std::istreambuf_iterator<char>()};
  std::ifstream log_quarantine_recovery_log_stream(log_quarantine_recovery.string() + ".log", std::ios::binary);
  const std::string log_quarantine_recovery_log_content{
      std::istreambuf_iterator<char>(log_quarantine_recovery_log_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      !log_quarantine_first.ok() && log_quarantine_restart.ok() && log_quarantine_restart->size() == 1 &&
          log_quarantine_recovery_content == "trusted log-quarantine video" &&
          log_quarantine_recovery_log_content == "trusted log-quarantine log",
      "Restart after log-sidecar quarantine must preserve authoritative video/log guards and finish recovery");

  const fs::path reconciliation_race_dir = root / "archive-cleanup-reconciliation-race";
  fs::create_directories(reconciliation_race_dir);
  const fs::path reconciliation_race_configured = reconciliation_race_dir / "reconcile-race.mkv";
  const fs::path reconciliation_race_source = reconciliation_race_dir / "reconcile-race.hstream-run-99999999-dead.mkv";
  std::ofstream(reconciliation_race_source, std::ios::binary) << "trusted reconciliation-race video";
  g_setenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_QUARANTINE", "1", TRUE);
  const auto reconciliation_race_first =
      hm::configurator_internal::recover_stale_archive_work_files(reconciliation_race_configured);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_QUARANTINE");
  g_setenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_PUBLIC_DURING_CLEANUP_RECONCILIATION", "1", TRUE);
  const auto reconciliation_race_restart =
      hm::configurator_internal::recover_stale_archive_work_files(reconciliation_race_configured);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_PUBLIC_DURING_CLEANUP_RECONCILIATION");
  bool reconciliation_race_rescued = false;
  bool reconciliation_race_foreign_retained = false;
  bool reconciliation_race_private_links_retired = false;
  fs::path reconciliation_race_foreign_path;
  for (const fs::path& entry : fs::directory_iterator(reconciliation_race_dir)) {
    if (fs::is_directory(entry) && entry.filename().string().rfind(".hstream-cleanup-v2-", 0) == 0) {
      reconciliation_race_private_links_retired = fs::exists(entry / "owner") && !fs::exists(entry / "entry") &&
          !fs::exists(entry / "guard") && !fs::exists(entry / "fallback");
      continue;
    }
    if (!fs::is_regular_file(entry))
      continue;
    std::ifstream entry_stream(entry, std::ios::binary);
    const std::string content{std::istreambuf_iterator<char>(entry_stream), std::istreambuf_iterator<char>()};
    reconciliation_race_rescued |= entry.filename().string().find(".hstream-reconcile-") != std::string::npos &&
        content == "trusted reconciliation-race video";
    if (content == "foreign public cleanup identity") {
      reconciliation_race_foreign_retained = true;
      reconciliation_race_foreign_path = entry;
    }
  }
  ok &= expect(
      !reconciliation_race_first.ok() && !reconciliation_race_restart.ok() && reconciliation_race_rescued &&
          reconciliation_race_foreign_retained && reconciliation_race_private_links_retired,
      "Cleanup reconciliation must retain a dedicated trusted guard through private-link retirement if the public path is replaced");
  const bool reconciliation_race_foreign_removed =
      !reconciliation_race_foreign_path.empty() && fs::remove(reconciliation_race_foreign_path);
  g_setenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_FALLBACK_QUARANTINE", "1", TRUE);
  const auto reconciliation_guard_retirement_interrupted =
      hm::configurator_internal::recover_stale_archive_work_files(reconciliation_race_configured);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_FALLBACK_QUARANTINE");
  bool reconciliation_guard_nested_transaction = false;
  bool reconciliation_outer_owner_only = false;
  bool reconciliation_guard_public_fallback = false;
  for (const fs::path& entry : fs::directory_iterator(reconciliation_race_dir)) {
    const std::string name = entry.filename().string();
    if (fs::is_directory(entry) && name.rfind(".hstream-cleanup-v2-", 0) == 0) {
      if (fs::exists(entry / "owner") && !fs::exists(entry / "entry") && !fs::exists(entry / "guard") &&
          !fs::exists(entry / "fallback")) {
        reconciliation_outer_owner_only = true;
      } else {
        reconciliation_guard_nested_transaction = true;
      }
    }
    reconciliation_guard_public_fallback |=
        fs::is_regular_file(entry) && name.find(".hstream-reconcile-") == 0 &&
        absl::EndsWith(name, ".hstream-cleanup-pin");
  }
  const auto reconciliation_race_resumed =
      hm::configurator_internal::recover_stale_archive_work_files(reconciliation_race_configured);
  bool reconciliation_race_artifacts_remain = false;
  for (const fs::path& entry : fs::directory_iterator(reconciliation_race_dir)) {
    const std::string name = entry.filename().string();
    reconciliation_race_artifacts_remain |=
        name.rfind(".hstream-cleanup-", 0) == 0 || name.find(".hstream-reconcile-") != std::string::npos;
  }
  std::ifstream reconciliation_race_recovery_stream(
      reconciliation_race_dir / "reconcile-race-finalization-failed.mkv", std::ios::binary);
  const std::string reconciliation_race_recovery_content{
      std::istreambuf_iterator<char>(reconciliation_race_recovery_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      reconciliation_race_foreign_removed && !reconciliation_guard_retirement_interrupted.ok() &&
          reconciliation_guard_nested_transaction && reconciliation_outer_owner_only &&
          !reconciliation_guard_public_fallback && reconciliation_race_resumed.ok() &&
          reconciliation_race_resumed->size() == 1 &&
          reconciliation_race_recovery_content == "trusted reconciliation-race video" &&
          !reconciliation_race_artifacts_remain,
      "Stable cleanup passes must resolve nested guard retirement before retiring its owner-only transaction");

  const fs::path deep_cleanup_chain_dir = root / "archive-deep-cleanup-chain";
  fs::create_directories(deep_cleanup_chain_dir);
  const std::array<std::string, 6> deep_cleanup_ids = {
      "00000000-0000-4000-8000-000000000010",
      "00000000-0000-4000-8000-000000000020",
      "00000000-0000-4000-8000-000000000030",
      "00000000-0000-4000-8000-000000000040",
      "00000000-0000-4000-8000-000000000050",
      "00000000-0000-4000-8000-000000000060",
  };
  bool deep_cleanup_chain_setup = true;
  for (size_t index = 0; index < deep_cleanup_ids.size(); ++index) {
    const fs::path transaction =
        deep_cleanup_chain_dir / (".hstream-cleanup-v2-" + deep_cleanup_ids[index]);
    fs::create_directories(transaction);
    const std::string target_name = index == 0
        ? "deep-cleanup-root.mkv"
        : ".hstream-reconcile-" + deep_cleanup_ids[index - 1] +
            "-target-ffffffff-ffff-4fff-8fff-ffffffffffff";
    gchar* encoded_target = g_base64_encode(
        reinterpret_cast<const guchar*>(target_name.data()), static_cast<gsize>(target_name.size()));
    std::ofstream owner_stream(transaction / "owner", std::ios::binary);
    owner_stream << "hstream-cleanup-v2\n" << encoded_target;
    owner_stream.close();
    deep_cleanup_chain_setup &= owner_stream.good();
    g_free(encoded_target);
  }
  const auto deep_cleanup_chain_recovery = hm::configurator_internal::recover_stale_archive_work_files(
      deep_cleanup_chain_dir / "configured.mkv");
  const bool deep_cleanup_chain_retired =
      std::none_of(fs::directory_iterator(deep_cleanup_chain_dir), fs::directory_iterator(), [](const auto& entry) {
        return entry.path().filename().string().rfind(".hstream-cleanup-v2-", 0) == 0;
      });
  ok &= expect(
      deep_cleanup_chain_setup && deep_cleanup_chain_recovery.ok() && deep_cleanup_chain_recovery->empty() &&
          deep_cleanup_chain_retired,
      "Cleanup reconciliation must reach a fixed point beyond the former five-pass dependency limit");

  const fs::path blocked_cleanup_chain_dir = root / "archive-blocked-cleanup-chain";
  const std::string blocked_outer_id = "00000000-0000-4000-8000-000000000070";
  const std::string blocked_nested_id = "00000000-0000-4000-8000-000000000080";
  const fs::path blocked_outer = blocked_cleanup_chain_dir / (".hstream-cleanup-v2-" + blocked_outer_id);
  const fs::path blocked_nested = blocked_cleanup_chain_dir / (".hstream-cleanup-v2-" + blocked_nested_id);
  fs::create_directories(blocked_outer);
  fs::create_directories(blocked_nested);
  const std::string blocked_nested_target =
      ".hstream-reconcile-" + blocked_outer_id + "-target-ffffffff-ffff-4fff-8fff-ffffffffffff";
  gchar* blocked_outer_target = g_base64_encode(
      reinterpret_cast<const guchar*>("blocked-cleanup-root.mkv"), std::strlen("blocked-cleanup-root.mkv"));
  gchar* blocked_nested_target_encoded = g_base64_encode(
      reinterpret_cast<const guchar*>(blocked_nested_target.data()), static_cast<gsize>(blocked_nested_target.size()));
  std::ofstream(blocked_outer / "owner", std::ios::binary) << "hstream-cleanup-v2\n" << blocked_outer_target;
  std::ofstream(blocked_nested / "owner", std::ios::binary)
      << "hstream-cleanup-v2\n"
      << blocked_nested_target_encoded;
  g_free(blocked_outer_target);
  g_free(blocked_nested_target_encoded);
  const int blocked_nested_fd =
      ::open(blocked_nested.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  const bool blocked_cleanup_chain_locked =
      blocked_nested_fd >= 0 && ::flock(blocked_nested_fd, LOCK_EX | LOCK_NB) == 0;
  const auto blocked_cleanup_chain_recovery = hm::configurator_internal::recover_stale_archive_work_files(
      blocked_cleanup_chain_dir / "configured.mkv");
  const bool blocked_cleanup_chain_retained = fs::exists(blocked_outer) && fs::exists(blocked_nested);
  if (blocked_nested_fd >= 0)
    ::close(blocked_nested_fd);
  const auto blocked_cleanup_chain_resumed = hm::configurator_internal::recover_stale_archive_work_files(
      blocked_cleanup_chain_dir / "configured.mkv");
  ok &= expect(
      blocked_cleanup_chain_locked && !blocked_cleanup_chain_recovery.ok() && blocked_cleanup_chain_retained &&
          blocked_cleanup_chain_resumed.ok() && blocked_cleanup_chain_resumed->empty() &&
          !fs::exists(blocked_outer) && !fs::exists(blocked_nested),
      "Cleanup reconciliation must fail closed at a blocked fixed point and resume after the blocker releases");

  const fs::path fallback_restore_race_dir = root / "archive-cleanup-fallback-restore-race";
  fs::create_directories(fallback_restore_race_dir);
  const fs::path fallback_restore_race_configured = fallback_restore_race_dir / "restore-race.mkv";
  const fs::path fallback_restore_race_source =
      fallback_restore_race_dir / "restore-race.hstream-run-99999999-dead.mkv";
  const fs::path fallback_restore_race_public = fallback_restore_race_source.string() + ".hstream-cleanup-pin";
  std::ofstream(fallback_restore_race_public, std::ios::binary) << "trusted cleanup fallback restore inode";
  const auto fallback_restore_race =
      hm::configurator_internal::recover_stale_archive_work_files(fallback_restore_race_configured);
  std::ifstream fallback_restore_race_public_stream(fallback_restore_race_public, std::ios::binary);
  const std::string fallback_restore_race_public_content{
      std::istreambuf_iterator<char>(fallback_restore_race_public_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      fallback_restore_race.ok() && fallback_restore_race->empty() && !fs::exists(fallback_restore_race_source) &&
          fallback_restore_race_public_content == "trusted cleanup fallback restore inode",
      "Recovery must not claim a cleanup-suffix lookalike without an authenticated cleanup owner record");

  const fs::path source_link_race_dir = root / "archive-source-link-race";
  fs::create_directories(source_link_race_dir);
  const fs::path source_link_race_source = source_link_race_dir / "source-link-race.mkv";
  const fs::path source_link_race_recovery = source_link_race_dir / "source-link-race-finalization-failed.mkv";
  std::ofstream(source_link_race_source, std::ios::binary) << "trusted source pinned before recovery link";
  g_setenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_SOURCE_BEFORE_LINK", "1", TRUE);
  const auto source_link_race = hm::configurator_internal::preserve_existing_archive_work_file(source_link_race_source);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_SOURCE_BEFORE_LINK");
  std::ifstream source_link_race_source_stream(source_link_race_source, std::ios::binary);
  const std::string source_link_race_source_content{
      std::istreambuf_iterator<char>(source_link_race_source_stream), std::istreambuf_iterator<char>()};
  std::ifstream source_link_race_recovery_stream(source_link_race_recovery, std::ios::binary);
  const std::string source_link_race_recovery_content{
      std::istreambuf_iterator<char>(source_link_race_recovery_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      !source_link_race.ok() && source_link_race_source_content == "injected foreign archive source before link" &&
          source_link_race_recovery_content == "trusted source pinned before recovery link" &&
          !fs::exists(source_link_race_recovery.string() + ".log"),
      "Recovery publication must link the pinned source inode and leave a replacement source pathname untouched");

  const auto interrupted_after_source_cleanup_is_reconciled =
      [&](const std::string& name, bool has_log, bool remove_visible_video) {
        const fs::path interrupted_dir = root / ("archive-post-source-cleanup-" + name);
        fs::create_directories(interrupted_dir);
        const fs::path configured = interrupted_dir / (name + ".mkv");
        const fs::path source = interrupted_dir / (name + ".hstream-run-99999999-dead.mkv");
        const fs::path source_log = source.string() + ".log";
        const fs::path recovery = interrupted_dir / (name + "-finalization-failed.mkv");
        const fs::path recovery_log = recovery.string() + ".log";
        std::ofstream(source, std::ios::binary) << name << " trusted video";
        if (has_log)
          std::ofstream(source_log, std::ios::binary) << name << " trusted log";
        g_setenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_SOURCE_CLEANUP", "1", TRUE);
        const auto interrupted = hm::configurator_internal::recover_stale_archive_work_files(configured);
        g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_SOURCE_CLEANUP");
        if (remove_visible_video)
          fs::remove(recovery);
        const auto resumed = hm::configurator_internal::recover_stale_archive_work_files(configured);
        const fs::path expected_recovery =
            remove_visible_video && !has_log ? interrupted_dir / (name + "-finalization-failed-1.mkv") : recovery;
        const fs::path expected_recovery_log = expected_recovery.string() + ".log";
        std::ifstream recovery_log_stream(expected_recovery_log, std::ios::binary);
        const std::string recovery_log_content{
            std::istreambuf_iterator<char>(recovery_log_stream), std::istreambuf_iterator<char>()};
        const bool result = !interrupted.ok() && resumed.ok() && resumed->size() == 1 &&
            resumed->front() == expected_recovery && fs::is_regular_file(expected_recovery) && !fs::exists(source) &&
            !fs::exists(source_log) &&
            (has_log ? recovery_log_content == name + " trusted log"
                     : !fs::exists(expected_recovery_log) && (!remove_visible_video || !fs::exists(recovery_log)));
        if (!result) {
          std::cerr << "source-cleanup reconciliation " << name << " interrupted=" << interrupted.status()
                    << " resumed=" << resumed.status() << " size=" << (resumed.ok() ? resumed->size() : 0)
                    << " expected=" << expected_recovery << " log=" << recovery_log_content << '\n';
        }
        return result;
      };
  ok &= expect(
      interrupted_after_source_cleanup_is_reconciled("with-log", true, false) &&
          interrupted_after_source_cleanup_is_reconciled("without-log", false, false) &&
          interrupted_after_source_cleanup_is_reconciled("guard-only-with-log", true, true) &&
          interrupted_after_source_cleanup_is_reconciled("guard-only-without-log", false, true),
      "Restart recovery must finish source-cleanup interruptions, including when only recovery guards remain");

  const fs::path replaced_recovery_guard_dir = root / "archive-replaced-recovery-video-guard";
  fs::create_directories(replaced_recovery_guard_dir);
  const fs::path replaced_recovery_guard_configured = replaced_recovery_guard_dir / "replaced-guard.mkv";
  const fs::path replaced_recovery_guard_source =
      replaced_recovery_guard_dir / "replaced-guard.hstream-run-99999999-dead.mkv";
  const fs::path replaced_recovery_guard_source_log = replaced_recovery_guard_source.string() + ".log";
  const fs::path replaced_recovery_guard_recovery =
      replaced_recovery_guard_dir / "replaced-guard-finalization-failed.mkv";
  const fs::path replaced_recovery_guard_recovery_log = replaced_recovery_guard_recovery.string() + ".log";
  const fs::path replaced_recovery_guard_path = replaced_recovery_guard_recovery.string() + ".hstream-pin";
  std::ofstream(replaced_recovery_guard_source, std::ios::binary) << "trusted replaced-guard video";
  std::ofstream(replaced_recovery_guard_source_log, std::ios::binary) << "trusted replaced-guard log";
  g_setenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_SOURCE_CLEANUP", "1", TRUE);
  const auto replaced_recovery_guard_interrupted =
      hm::configurator_internal::recover_stale_archive_work_files(replaced_recovery_guard_configured);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_SOURCE_CLEANUP");
  fs::remove(replaced_recovery_guard_path);
  std::ofstream(replaced_recovery_guard_path, std::ios::binary) << "foreign replaced recovery video guard";
  const auto replaced_recovery_guard_restart =
      hm::configurator_internal::recover_stale_archive_work_files(replaced_recovery_guard_configured);
  std::ifstream replaced_recovery_guard_video_stream(replaced_recovery_guard_recovery, std::ios::binary);
  const std::string replaced_recovery_guard_video{
      std::istreambuf_iterator<char>(replaced_recovery_guard_video_stream), std::istreambuf_iterator<char>()};
  std::ifstream replaced_recovery_guard_log_stream(replaced_recovery_guard_recovery_log, std::ios::binary);
  const std::string replaced_recovery_guard_log{
      std::istreambuf_iterator<char>(replaced_recovery_guard_log_stream), std::istreambuf_iterator<char>()};
  std::ifstream replaced_recovery_guard_foreign_stream(replaced_recovery_guard_path, std::ios::binary);
  const std::string replaced_recovery_guard_foreign{
      std::istreambuf_iterator<char>(replaced_recovery_guard_foreign_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      !replaced_recovery_guard_interrupted.ok() && !replaced_recovery_guard_restart.ok() &&
          replaced_recovery_guard_video == "trusted replaced-guard video" &&
          replaced_recovery_guard_log == "trusted replaced-guard log" &&
          replaced_recovery_guard_foreign == "foreign replaced recovery video guard" &&
          fs::exists(replaced_recovery_guard_source.string() + ".hstream-pin") &&
          fs::exists(replaced_recovery_guard_source_log.string() + ".hstream-pin") &&
          !fs::exists(replaced_recovery_guard_dir / "replaced-guard-finalization-failed-1.mkv"),
      "Restart must reject a replaced recovery video guard instead of pairing it with the trusted source log");

  const fs::path replaced_recovery_log_guard_dir = root / "archive-replaced-recovery-log-guard";
  fs::create_directories(replaced_recovery_log_guard_dir);
  const fs::path replaced_recovery_log_guard_configured = replaced_recovery_log_guard_dir / "replaced-log-guard.mkv";
  const fs::path replaced_recovery_log_guard_source =
      replaced_recovery_log_guard_dir / "replaced-log-guard.hstream-run-99999999-dead.mkv";
  const fs::path replaced_recovery_log_guard_source_log = replaced_recovery_log_guard_source.string() + ".log";
  const fs::path replaced_recovery_log_guard_recovery =
      replaced_recovery_log_guard_dir / "replaced-log-guard-finalization-failed.mkv";
  const fs::path replaced_recovery_log_guard_recovery_log = replaced_recovery_log_guard_recovery.string() + ".log";
  const fs::path replaced_recovery_log_guard_path = replaced_recovery_log_guard_recovery_log.string() + ".hstream-pin";
  std::ofstream(replaced_recovery_log_guard_source, std::ios::binary) << "trusted replaced-log-guard video";
  std::ofstream(replaced_recovery_log_guard_source_log, std::ios::binary) << "trusted replaced-log-guard log";
  g_setenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_SOURCE_CLEANUP", "1", TRUE);
  const auto replaced_recovery_log_guard_interrupted =
      hm::configurator_internal::recover_stale_archive_work_files(replaced_recovery_log_guard_configured);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_SOURCE_CLEANUP");
  fs::remove(replaced_recovery_log_guard_path);
  std::ofstream(replaced_recovery_log_guard_path, std::ios::binary) << "foreign replaced recovery log guard";
  const auto replaced_recovery_log_guard_restart =
      hm::configurator_internal::recover_stale_archive_work_files(replaced_recovery_log_guard_configured);
  std::ifstream replaced_recovery_log_guard_video_stream(replaced_recovery_log_guard_recovery, std::ios::binary);
  const std::string replaced_recovery_log_guard_video{
      std::istreambuf_iterator<char>(replaced_recovery_log_guard_video_stream), std::istreambuf_iterator<char>()};
  std::ifstream replaced_recovery_log_guard_log_stream(replaced_recovery_log_guard_recovery_log, std::ios::binary);
  const std::string replaced_recovery_log_guard_log{
      std::istreambuf_iterator<char>(replaced_recovery_log_guard_log_stream), std::istreambuf_iterator<char>()};
  std::ifstream replaced_recovery_log_guard_foreign_stream(replaced_recovery_log_guard_path, std::ios::binary);
  const std::string replaced_recovery_log_guard_foreign{
      std::istreambuf_iterator<char>(replaced_recovery_log_guard_foreign_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      !replaced_recovery_log_guard_interrupted.ok() && !replaced_recovery_log_guard_restart.ok() &&
          replaced_recovery_log_guard_video == "trusted replaced-log-guard video" &&
          replaced_recovery_log_guard_log == "trusted replaced-log-guard log" &&
          replaced_recovery_log_guard_foreign == "foreign replaced recovery log guard" &&
          fs::exists(replaced_recovery_log_guard_source.string() + ".hstream-pin") &&
          fs::exists(replaced_recovery_log_guard_source_log.string() + ".hstream-pin") &&
          !fs::exists(replaced_recovery_log_guard_dir / "replaced-log-guard-finalization-failed-1.mkv"),
      "Restart must reject a replaced recovery log guard instead of pairing it with the trusted source video");

  const fs::path ui_guard_boundary_dir = root / "archive-ui-guard-retirement-boundary";
  fs::create_directories(ui_guard_boundary_dir);
  const fs::path ui_guard_boundary_configured = ui_guard_boundary_dir / "ui-boundary.mkv";
  const fs::path ui_guard_boundary_source = ui_guard_boundary_dir / "ui-boundary.hstream-run-99999999-dead.mkv";
  const fs::path ui_guard_boundary_source_log = ui_guard_boundary_source.string() + ".log";
  const fs::path ui_guard_boundary_recovery = ui_guard_boundary_dir / "ui-boundary-finalization-failed.mkv";
  const fs::path ui_guard_boundary_recovery_log = ui_guard_boundary_recovery.string() + ".log";
  const fs::path ui_guard_boundary_video_backing = ui_guard_boundary_dir / "ui-boundary-video-backing";
  const fs::path ui_guard_boundary_log_backing = ui_guard_boundary_dir / "ui-boundary-log-backing";
  std::ofstream(ui_guard_boundary_video_backing, std::ios::binary) << "trusted UI-boundary video";
  std::ofstream(ui_guard_boundary_log_backing, std::ios::binary) << "trusted UI-boundary log";
  fs::create_hard_link(ui_guard_boundary_video_backing, ui_guard_boundary_recovery);
  fs::create_hard_link(ui_guard_boundary_log_backing, ui_guard_boundary_recovery_log);
  fs::create_hard_link(ui_guard_boundary_video_backing, ui_guard_boundary_recovery.string() + ".hstream-pin");
  fs::create_hard_link(ui_guard_boundary_log_backing, ui_guard_boundary_recovery_log.string() + ".hstream-pin");
  fs::create_hard_link(ui_guard_boundary_log_backing, ui_guard_boundary_source_log.string() + ".hstream-pin");
  const auto ui_guard_boundary_restart =
      hm::configurator_internal::recover_stale_archive_work_files(ui_guard_boundary_configured);
  ok &= expect(
      ui_guard_boundary_restart.ok() && ui_guard_boundary_restart->size() == 1 &&
          ui_guard_boundary_restart->front() == ui_guard_boundary_recovery &&
          !fs::exists(ui_guard_boundary_source.string() + ".hstream-pin") &&
          !fs::exists(ui_guard_boundary_source_log.string() + ".hstream-pin") &&
          !fs::exists(ui_guard_boundary_recovery.string() + ".hstream-pin") &&
          !fs::exists(ui_guard_boundary_recovery_log.string() + ".hstream-pin"),
      "Restart must finish the UI boundary after its source video guard retires before its source log guard");

  const auto late_interrupted_recovery_is_reconciled =
      [&](const std::string& name, bool has_log, const char* interruption_env, bool expect_reported = true) {
        const fs::path interrupted_dir = root / ("archive-late-interruption-" + name);
        fs::create_directories(interrupted_dir);
        const fs::path configured = interrupted_dir / (name + ".mkv");
        const fs::path source = interrupted_dir / (name + ".hstream-run-99999999-dead.mkv");
        const fs::path source_log = source.string() + ".log";
        const fs::path recovery = interrupted_dir / (name + "-finalization-failed.mkv");
        const fs::path recovery_log = recovery.string() + ".log";
        std::ofstream(source, std::ios::binary) << name << " late video";
        if (has_log)
          std::ofstream(source_log, std::ios::binary) << name << " late log";
        g_setenv(interruption_env, "1", TRUE);
        const auto interrupted = hm::configurator_internal::recover_stale_archive_work_files(configured);
        g_unsetenv(interruption_env);
        if (!expect_reported) {
          fs::remove(recovery);
          std::ofstream(recovery, std::ios::binary) << name << " foreign video after commit point";
        }
        const auto resumed = hm::configurator_internal::recover_stale_archive_work_files(configured);
        std::ifstream recovery_video_stream(recovery, std::ios::binary);
        const std::string recovery_video{
            std::istreambuf_iterator<char>(recovery_video_stream), std::istreambuf_iterator<char>()};
        std::ifstream recovery_log_stream(recovery_log, std::ios::binary);
        const std::string recovery_log_content{
            std::istreambuf_iterator<char>(recovery_log_stream), std::istreambuf_iterator<char>()};
        return !interrupted.ok() && resumed.ok() &&
            (expect_reported ? resumed->size() == 1 && resumed->front() == recovery : resumed->empty()) &&
            recovery_video == name + (expect_reported ? " late video" : " foreign video after commit point") &&
            !fs::exists(source) && !fs::exists(source_log) && !fs::exists(recovery.string() + ".hstream-pin") &&
            !fs::exists(recovery_log.string() + ".hstream-pin") &&
            (has_log ? recovery_log_content == name + " late log" : !fs::exists(recovery_log));
      };
  ok &= expect(
      late_interrupted_recovery_is_reconciled(
          "after-log-cleanup", true, "HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_LOG_CLEANUP") &&
          late_interrupted_recovery_is_reconciled(
              "after-marker-cleanup", false, "HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_LOG_CLEANUP") &&
          late_interrupted_recovery_is_reconciled(
              "during-guard-retirement",
              true,
              "HSTREAM_CONFIGURATOR_TEST_INTERRUPT_DURING_ARCHIVE_GUARD_RETIREMENT",
              false) &&
          late_interrupted_recovery_is_reconciled(
              "between-guard-pairs",
              true,
              "HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_RECOVERY_GUARD_RETIREMENT"),
      "Restart recovery must finish transactions interrupted after sidecar cleanup and during guard retirement");

  const fs::path between_guards_dir = root / "archive-between-guards";
  fs::create_directories(between_guards_dir);
  const fs::path between_guards_configured = between_guards_dir / "between.mkv";
  const fs::path between_guards_source = between_guards_dir / "between.hstream-run-99999999-dead.mkv";
  const fs::path between_guards_source_log = between_guards_source.string() + ".log";
  const fs::path between_guards_foreign_log = between_guards_dir / "between-finalization-failed.mkv.log";
  const fs::path between_guards_rescued = between_guards_dir / "between-finalization-failed-1.mkv";
  std::ofstream(between_guards_source, std::ios::binary) << "trusted between-guards video";
  std::ofstream(between_guards_source_log, std::ios::binary) << "trusted between-guards log";
  g_setenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_LOG_BETWEEN_GUARDS", "1", TRUE);
  const auto between_guards_interrupted =
      hm::configurator_internal::recover_stale_archive_work_files(between_guards_configured);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_LOG_BETWEEN_GUARDS");
  const auto between_guards_recovered =
      hm::configurator_internal::recover_stale_archive_work_files(between_guards_configured);
  std::ifstream between_guards_video_stream(between_guards_rescued, std::ios::binary);
  const std::string between_guards_video{
      std::istreambuf_iterator<char>(between_guards_video_stream), std::istreambuf_iterator<char>()};
  std::ifstream between_guards_log_stream(between_guards_rescued.string() + ".log", std::ios::binary);
  const std::string between_guards_log{
      std::istreambuf_iterator<char>(between_guards_log_stream), std::istreambuf_iterator<char>()};
  std::ifstream between_guards_foreign_log_stream(between_guards_foreign_log, std::ios::binary);
  const std::string between_guards_foreign_log_content{
      std::istreambuf_iterator<char>(between_guards_foreign_log_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      !between_guards_interrupted.ok() && between_guards_recovered.ok() && between_guards_recovered->size() == 1 &&
          between_guards_recovered->front() == between_guards_rescued &&
          between_guards_video == "trusted between-guards video" &&
          between_guards_log == "trusted between-guards log" &&
          between_guards_foreign_log_content == "injected foreign archive log between guard retirements",
      "Recovery guard retirement must rescue both trusted identities if the visible log is replaced between removals");

  const fs::path source_guard_restart_dir = root / "archive-source-guard-restart";
  fs::create_directories(source_guard_restart_dir);
  const fs::path source_guard_restart_configured = source_guard_restart_dir / "source-guard.mkv";
  const fs::path source_guard_restart_source = source_guard_restart_dir / "source-guard.hstream-run-99999999-dead.mkv";
  const fs::path source_guard_restart_source_log = source_guard_restart_source.string() + ".log";
  const fs::path source_guard_restart_recovery = source_guard_restart_dir / "source-guard-finalization-failed.mkv";
  const fs::path source_guard_restart_foreign_log = source_guard_restart_recovery.string() + ".log";
  const fs::path source_guard_restart_rescue = source_guard_restart_dir / "source-guard-finalization-failed-1.mkv";
  std::ofstream(source_guard_restart_source, std::ios::binary) << "trusted source-guard restart video";
  std::ofstream(source_guard_restart_source_log, std::ios::binary) << "trusted source-guard restart log";
  g_setenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_RECOVERY_GUARD_RETIREMENT", "1", TRUE);
  const auto source_guard_restart_interrupted =
      hm::configurator_internal::recover_stale_archive_work_files(source_guard_restart_configured);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_RECOVERY_GUARD_RETIREMENT");
  fs::remove(source_guard_restart_recovery);
  fs::remove(source_guard_restart_foreign_log);
  std::ofstream(source_guard_restart_foreign_log, std::ios::binary) << "foreign log after recovery guards retired";
  const auto source_guard_restart_recovered =
      hm::configurator_internal::recover_stale_archive_work_files(source_guard_restart_configured);
  std::ifstream source_guard_restart_video_stream(source_guard_restart_rescue, std::ios::binary);
  const std::string source_guard_restart_video{
      std::istreambuf_iterator<char>(source_guard_restart_video_stream), std::istreambuf_iterator<char>()};
  std::ifstream source_guard_restart_log_stream(source_guard_restart_rescue.string() + ".log", std::ios::binary);
  const std::string source_guard_restart_log{
      std::istreambuf_iterator<char>(source_guard_restart_log_stream), std::istreambuf_iterator<char>()};
  std::ifstream source_guard_restart_foreign_stream(source_guard_restart_foreign_log, std::ios::binary);
  const std::string source_guard_restart_foreign{
      std::istreambuf_iterator<char>(source_guard_restart_foreign_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      !source_guard_restart_interrupted.ok() && source_guard_restart_recovered.ok() &&
          source_guard_restart_recovered->size() == 1 &&
          source_guard_restart_recovered->front() == source_guard_restart_rescue &&
          source_guard_restart_video == "trusted source-guard restart video" &&
          source_guard_restart_log == "trusted source-guard restart log" &&
          source_guard_restart_foreign == "foreign log after recovery guards retired",
      "Restart must restore an absent source from its guards and rescue a recovery pair whose visible names vanished");

  const fs::path missing_log_guard_dir = root / "archive-missing-recovery-log-guard";
  fs::create_directories(missing_log_guard_dir);
  const fs::path missing_log_guard_configured = missing_log_guard_dir / "missing-log-guard.mkv";
  const fs::path missing_log_guard_source = missing_log_guard_dir / "missing-log-guard.hstream-run-99999999-dead.mkv";
  const fs::path missing_log_guard_source_log = missing_log_guard_source.string() + ".log";
  const fs::path missing_log_guard_recovery = missing_log_guard_dir / "missing-log-guard-finalization-failed.mkv";
  const fs::path missing_log_guard_recovery_log = missing_log_guard_recovery.string() + ".log";
  const fs::path missing_log_guard_video_backing = missing_log_guard_dir / "video-backing";
  const fs::path missing_log_guard_log_backing = missing_log_guard_dir / "log-backing";
  std::ofstream(missing_log_guard_video_backing, std::ios::binary) << "missing-log-guard trusted video";
  std::ofstream(missing_log_guard_log_backing, std::ios::binary) << "missing-log-guard trusted log";
  fs::create_hard_link(missing_log_guard_video_backing, missing_log_guard_recovery);
  fs::create_hard_link(missing_log_guard_log_backing, missing_log_guard_recovery_log);
  fs::create_hard_link(missing_log_guard_video_backing, missing_log_guard_recovery.string() + ".hstream-pin");
  fs::create_hard_link(missing_log_guard_video_backing, missing_log_guard_source.string() + ".hstream-pin");
  fs::create_hard_link(missing_log_guard_log_backing, missing_log_guard_source_log.string() + ".hstream-pin");
  const auto missing_log_guard_recovered =
      hm::configurator_internal::recover_stale_archive_work_files(missing_log_guard_configured);
  std::ifstream missing_log_guard_video_stream(missing_log_guard_recovery, std::ios::binary);
  const std::string missing_log_guard_video{
      std::istreambuf_iterator<char>(missing_log_guard_video_stream), std::istreambuf_iterator<char>()};
  std::ifstream missing_log_guard_log_stream(missing_log_guard_recovery_log, std::ios::binary);
  const std::string missing_log_guard_log{
      std::istreambuf_iterator<char>(missing_log_guard_log_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      missing_log_guard_recovered.ok() && missing_log_guard_recovered->size() == 1 &&
          missing_log_guard_recovered->front() == missing_log_guard_recovery &&
          missing_log_guard_video == "missing-log-guard trusted video" &&
          missing_log_guard_log == "missing-log-guard trusted log" &&
          !fs::exists(missing_log_guard_recovery.string() + ".hstream-pin") &&
          !fs::exists(missing_log_guard_recovery_log.string() + ".hstream-pin") &&
          !fs::exists(missing_log_guard_source.string() + ".hstream-pin") &&
          !fs::exists(missing_log_guard_source_log.string() + ".hstream-pin") &&
          !fs::exists(missing_log_guard_dir / "missing-log-guard-finalization-failed-1.mkv"),
      "Restart must reconstruct a missing recovery log guard from the surviving source guard without splitting the pair");

  const fs::path guard_only_log_dir = root / "archive-guard-only-log";
  fs::create_directories(guard_only_log_dir);
  const std::string guard_only_owner = std::to_string(::getpid()) + "-11223344-5566-7788-99aa-bbccddeeff00";
  const fs::path guard_only_configured = guard_only_log_dir / "guard-only-log.mkv";
  const fs::path guard_only_source =
      guard_only_log_dir / ("guard-only-log.hstream-run-v3-99999990-" + guard_only_owner + ".mkv");
  const fs::path guard_only_provisional_log =
      guard_only_log_dir / ("guard-only-log.hstream-run-ui-" + guard_only_owner + ".mkv.log");
  const fs::path guard_only_log_backing = guard_only_log_dir / "guard-only-log-backing";
  const fs::path guard_only_recovery = guard_only_log_dir / "guard-only-log-finalization-failed.mkv";
  std::ofstream(guard_only_source, std::ios::binary) << "guard-only log video";
  std::ofstream(guard_only_log_backing, std::ios::binary) << "guard-only trusted UI log";
  fs::create_hard_link(guard_only_log_backing, guard_only_provisional_log.string() + ".hstream-pin");
  const auto guard_only_log_recovered =
      hm::configurator_internal::recover_stale_archive_work_files(guard_only_configured);
  std::ifstream guard_only_recovery_log_stream(guard_only_recovery.string() + ".log", std::ios::binary);
  const std::string guard_only_recovery_log{
      std::istreambuf_iterator<char>(guard_only_recovery_log_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      guard_only_log_recovered.ok() && guard_only_log_recovered->size() == 1 &&
          guard_only_log_recovered->front() == guard_only_recovery &&
          guard_only_recovery_log == "guard-only trusted UI log" && !fs::exists(guard_only_source) &&
          !fs::exists(guard_only_provisional_log) && !fs::exists(guard_only_provisional_log.string() + ".hstream-pin"),
      "Recovery must republish and pair a UI log whose durable guard is its only surviving conventional name");

  const fs::path reconstructed_collision_dir = root / "archive-reconstructed-guard-collision";
  fs::create_directories(reconstructed_collision_dir);
  const fs::path reconstructed_configured = reconstructed_collision_dir / "reconstructed.mkv";
  const fs::path reconstructed_source = reconstructed_collision_dir / "reconstructed.hstream-run-99999999-dead.mkv";
  const fs::path reconstructed_source_log = reconstructed_source.string() + ".log";
  const fs::path reconstructed_recovery = reconstructed_collision_dir / "reconstructed-finalization-failed.mkv";
  const fs::path reconstructed_recovery_log = reconstructed_recovery.string() + ".log";
  const fs::path reconstructed_expected = reconstructed_collision_dir / "reconstructed-finalization-failed-1.mkv";
  std::ofstream(reconstructed_source, std::ios::binary) << "reconstructed trusted video";
  std::ofstream(reconstructed_source_log, std::ios::binary) << "reconstructed trusted log";
  fs::create_hard_link(reconstructed_source, reconstructed_source.string() + ".hstream-pin");
  fs::create_hard_link(reconstructed_source_log, reconstructed_source_log.string() + ".hstream-pin");
  fs::create_hard_link(reconstructed_source, reconstructed_recovery);
  std::ofstream(reconstructed_recovery_log, std::ios::binary) << "foreign reconstructed collision log";
  const auto reconstructed_recovered =
      hm::configurator_internal::recover_stale_archive_work_files(reconstructed_configured);
  const auto reconstructed_second_pass =
      hm::configurator_internal::recover_stale_archive_work_files(reconstructed_configured);
  std::ifstream reconstructed_video_stream(reconstructed_expected, std::ios::binary);
  const std::string reconstructed_video{
      std::istreambuf_iterator<char>(reconstructed_video_stream), std::istreambuf_iterator<char>()};
  std::ifstream reconstructed_log_stream(reconstructed_expected.string() + ".log", std::ios::binary);
  const std::string reconstructed_log{
      std::istreambuf_iterator<char>(reconstructed_log_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      reconstructed_recovered.ok() && reconstructed_recovered->size() == 1 &&
          reconstructed_recovered->front() == reconstructed_expected && reconstructed_second_pass.ok() &&
          reconstructed_second_pass->empty() && reconstructed_video == "reconstructed trusted video" &&
          reconstructed_log == "reconstructed trusted log" &&
          !fs::exists(reconstructed_recovery.string() + ".hstream-pin") &&
          !fs::exists(reconstructed_recovery_log.string() + ".hstream-pin"),
      "Collision handling must retire reconstructed guards while the durable source pair moves to one new recovery");

  const auto partial_rescue_is_resumed = [&](const std::string& name,
                                             const char* interruption_env,
                                             bool remove_superseded_log = false) {
    const fs::path interrupted_dir = root / ("archive-partial-rescue-" + name);
    fs::create_directories(interrupted_dir);
    const fs::path configured = interrupted_dir / (name + ".mkv");
    const fs::path source = interrupted_dir / (name + ".hstream-run-99999999-dead.mkv");
    const fs::path source_log = source.string() + ".log";
    const fs::path recovery = interrupted_dir / (name + "-finalization-failed.mkv");
    const fs::path rescued = interrupted_dir / (name + "-finalization-failed-1.mkv");
    std::ofstream(source, std::ios::binary) << name << " partial rescue video";
    std::ofstream(source_log, std::ios::binary) << name << " partial rescue log";
    g_setenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_SOURCE_CLEANUP", "1", TRUE);
    const auto source_cleanup_interrupted = hm::configurator_internal::recover_stale_archive_work_files(configured);
    g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_SOURCE_CLEANUP");
    fs::remove(recovery);
    std::ofstream(recovery, std::ios::binary) << name << " foreign recovery video";
    g_setenv(interruption_env, "1", TRUE);
    const auto rescue_interrupted = hm::configurator_internal::recover_stale_archive_work_files(configured);
    g_unsetenv(interruption_env);
    if (remove_superseded_log)
      fs::remove(recovery.string() + ".log");
    const auto resumed = hm::configurator_internal::recover_stale_archive_work_files(configured);
    const auto settled = hm::configurator_internal::recover_stale_archive_work_files(configured);
    std::ifstream rescued_video_stream(rescued, std::ios::binary);
    const std::string rescued_video{
        std::istreambuf_iterator<char>(rescued_video_stream), std::istreambuf_iterator<char>()};
    std::ifstream rescued_log_stream(rescued.string() + ".log", std::ios::binary);
    const std::string rescued_log{std::istreambuf_iterator<char>(rescued_log_stream), std::istreambuf_iterator<char>()};
    const bool result = !source_cleanup_interrupted.ok() && !rescue_interrupted.ok() && resumed.ok() &&
        resumed->size() == 1 && resumed->front() == rescued && settled.ok() && settled->empty() &&
        rescued_video == name + " partial rescue video" && rescued_log == name + " partial rescue log" &&
        !fs::exists(rescued.string() + ".hstream-pin") && !fs::exists(rescued.string() + ".log.hstream-pin") &&
        !fs::exists(recovery.string() + ".log") && !fs::exists(recovery.string() + ".log.hstream-pin");
    if (!result) {
      std::cerr << "partial rescue " << name << " source-interrupt=" << source_cleanup_interrupted.status()
                << " rescue-interrupt=" << rescue_interrupted.status() << " resumed=" << resumed.status()
                << " resumed-size=" << (resumed.ok() ? resumed->size() : 0) << " settled=" << settled.status()
                << " settled-size=" << (settled.ok() ? settled->size() : 0) << " video=" << rescued_video
                << " log=" << rescued_log << '\n';
    }
    return result;
  };
  ok &= expect(
      partial_rescue_is_resumed("after-log-guard", "HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_RESCUE_LOG_GUARD") &&
          partial_rescue_is_resumed(
              "after-video-guard", "HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_RESCUE_VIDEO_GUARD") &&
          partial_rescue_is_resumed("after-log-link", "HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_RESCUE_LOG_LINK") &&
          partial_rescue_is_resumed(
              "after-video-link", "HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_RESCUE_VIDEO_LINK") &&
          partial_rescue_is_resumed(
              "between-old-guards", "HSTREAM_CONFIGURATOR_TEST_INTERRUPT_BETWEEN_SUPERSEDED_GUARD_REMOVALS", true),
      "Restart must resume rescue publication after every link and after loss of a superseded visible pathname");

  const fs::path finalizer_archive = custom_archive_dir / "finalizer-ownership.mkv";
  const fs::path finalizer_work = custom_archive_dir /
      ("finalizer-ownership.hstream-run-v3-99999996-" + std::to_string(::getpid()) +
       "-11223344-5566-7788-99aa-bbccddeeff00.mkv");
  std::ofstream(finalizer_work, std::ios::binary) << "work file being consumed by the UI finalizer";
  const auto finalizer_lock = hm::configurator_internal::acquire_archive_work_owner_lock(finalizer_work);
  pid_t recovery_probe = -1;
  if (finalizer_lock.ok())
    recovery_probe = ::fork();
  if (recovery_probe == 0) {
    ::close(*finalizer_lock);
    const auto during_finalization = hm::configurator_internal::recover_stale_archive_work_files(finalizer_archive);
    _exit(during_finalization.ok() && during_finalization->empty() && fs::is_regular_file(finalizer_work) ? 0 : 1);
  }
  int recovery_probe_status = -1;
  const bool recovery_probe_passed = recovery_probe > 0 && ::waitpid(recovery_probe, &recovery_probe_status, 0) > 0 &&
      WIFEXITED(recovery_probe_status) && WEXITSTATUS(recovery_probe_status) == 0;
  if (finalizer_lock.ok())
    ::close(*finalizer_lock);
  const auto before_ui_relinquishes = hm::configurator_internal::recover_stale_archive_work_files(finalizer_archive);
  const bool preserved_before_relinquish = fs::is_regular_file(finalizer_work);
  fs::remove(hm::configurator_internal::archive_work_owner_lock_path(finalizer_work));
  const auto after_finalization = hm::configurator_internal::recover_stale_archive_work_files(finalizer_archive);
  ok &= expect(
      finalizer_lock.ok() && recovery_probe_passed && before_ui_relinquishes.ok() && before_ui_relinquishes->empty() &&
          preserved_before_relinquish && after_finalization.ok() && after_finalization->size() == 1 &&
          !fs::exists(finalizer_work) && fs::is_regular_file(after_finalization->front()) &&
          !fs::exists(hm::configurator_internal::archive_work_owner_lock_path(finalizer_work)),
      "A second backend must preserve work during backend-to-UI ownership transfer and finalization, then recover it after explicit relinquishment");

  const fs::path failed_start_archive = custom_archive_dir / "failed-start.mkv";
  const fs::path failed_start_work = custom_archive_dir /
      ("failed-start.hstream-run-v3-99999995-" + std::to_string(::getpid()) +
       "-22334455-6677-8899-aabb-ccddeeff0011.mkv");
  std::ofstream(failed_start_work, std::ios::binary);
  const auto failed_start_lock = hm::configurator_internal::acquire_archive_work_owner_lock(failed_start_work);
  if (failed_start_lock.ok())
    ::close(*failed_start_lock);
  const fs::path failed_start_lock_path = hm::configurator_internal::archive_work_owner_lock_path(failed_start_work);
  const bool failed_start_relinquished = fs::remove(failed_start_lock_path);
  const auto failed_start_cleanup = hm::configurator_internal::recover_stale_archive_work_files(failed_start_archive);
  std::ofstream(failed_start_work, std::ios::binary);
  const auto repeated_failed_start_lock = hm::configurator_internal::acquire_archive_work_owner_lock(failed_start_work);
  if (repeated_failed_start_lock.ok())
    ::close(*repeated_failed_start_lock);
  const bool repeated_failed_start_relinquished = fs::remove(failed_start_lock_path);
  const auto repeated_failed_start_cleanup =
      hm::configurator_internal::recover_stale_archive_work_files(failed_start_archive);
  ok &= expect(
      failed_start_lock.ok() && failed_start_relinquished && failed_start_cleanup.ok() &&
          failed_start_cleanup->empty() && repeated_failed_start_lock.ok() && repeated_failed_start_relinquished &&
          repeated_failed_start_cleanup.ok() && repeated_failed_start_cleanup->empty() &&
          !fs::exists(failed_start_work) && !fs::exists(failed_start_lock_path),
      "Repeated failed starts must durably clean each relinquished zero-byte v3 reservation and ownership sidecar");

  const fs::path replaced_lock_archive = custom_archive_dir / "replaced-lock.mkv";
  const fs::path replaced_lock_work =
      custom_archive_dir / "replaced-lock.hstream-run-v3-99999992-99999991-33445566-7788-99aa-bbcc-ddeeff001122.mkv";
  std::ofstream(replaced_lock_work, std::ios::binary);
  const auto replaced_lock = hm::configurator_internal::acquire_archive_work_owner_lock(replaced_lock_work);
  if (replaced_lock.ok())
    ::close(*replaced_lock);
  const fs::path replaced_lock_path = hm::configurator_internal::archive_work_owner_lock_path(replaced_lock_work);
  g_setenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_OWNER_LOCK", "1", TRUE);
  const auto replaced_lock_cleanup = hm::configurator_internal::recover_stale_archive_work_files(replaced_lock_archive);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_OWNER_LOCK");
  std::ifstream replaced_lock_stream(replaced_lock_path, std::ios::binary);
  const std::string replaced_lock_content{
      std::istreambuf_iterator<char>(replaced_lock_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      replaced_lock.ok() && !replaced_lock_cleanup.ok() && !fs::exists(replaced_lock_work) &&
          replaced_lock_content == "injected foreign archive owner lock",
      "Stale recovery must never delete a replacement ownership-lock entry after acquiring the original inode");

  const fs::path replaced_reservation_archive = custom_archive_dir / "replaced-reservation.mkv";
  const fs::path replaced_reservation_work = custom_archive_dir /
      "replaced-reservation.hstream-run-v3-99999990-99999989-44556677-8899-aabb-ccdd-eeff00112233.mkv";
  std::ofstream(replaced_reservation_work, std::ios::binary);
  const auto replaced_reservation_lock =
      hm::configurator_internal::acquire_archive_work_owner_lock(replaced_reservation_work);
  if (replaced_reservation_lock.ok())
    ::close(*replaced_reservation_lock);
  const fs::path replaced_reservation_lock_path =
      hm::configurator_internal::archive_work_owner_lock_path(replaced_reservation_work);
  g_setenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_RESERVATION_BEFORE_CLEANUP", "1", TRUE);
  const auto replaced_reservation_cleanup =
      hm::configurator_internal::recover_stale_archive_work_files(replaced_reservation_archive);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_RESERVATION_BEFORE_CLEANUP");
  std::ifstream replaced_reservation_stream(replaced_reservation_work, std::ios::binary);
  const std::string replaced_reservation_content{
      std::istreambuf_iterator<char>(replaced_reservation_stream), std::istreambuf_iterator<char>()};
  ok &= expect(
      replaced_reservation_lock.ok() && !replaced_reservation_cleanup.ok() &&
          replaced_reservation_content == "injected foreign archive reservation" &&
          fs::is_regular_file(replaced_reservation_lock_path),
      "A replaced zero-byte reservation must retain its ownership sidecar when identity-checked cleanup fails");

  const auto first_archive_lock = hm::configurator_internal::acquire_archive_output_lock(custom_archive);
  const auto conflicting_archive_lock = hm::configurator_internal::acquire_archive_output_lock(custom_archive);
  ok &= expect(
      first_archive_lock.ok() && !conflicting_archive_lock.ok() &&
          conflicting_archive_lock.status().code() == absl::StatusCode::kAlreadyExists,
      "A second backend must fail cleanly instead of renaming or sharing a live direct-CLI archive");
  if (first_archive_lock.ok())
    ::close(*first_archive_lock);
  const auto reacquired_archive_lock = hm::configurator_internal::acquire_archive_output_lock(custom_archive);
  ok &= expect(
      reacquired_archive_lock.ok(), "Archive ownership lock must become available after the prior backend exits");
  if (reacquired_archive_lock.ok())
    ::close(*reacquired_archive_lock);

  const fs::path superseded_force_dir = games / "superseded-force";
  fs::create_directories(superseded_force_dir);
  std::ofstream(superseded_force_dir / "seam_file.png") << "newer generation\n";
  YAML::Node stale_force_config(YAML::NodeType::Map);
  stale_force_config["pipeline"]["application"]["complete-configuration"] = "1";
  stale_force_config["pipeline"]["hmstitcher"]["enable"] = "1";
  stale_force_config["hstream_ui"]["stitching_calibration"]["control_points"] = 1500;
  stale_force_config["hstream_ui"]["stitching_calibration"]["status"] = "pending";
  stale_force_config["hstream_ui"]["stitching_calibration"]["stale_from"] = "input";
  stale_force_config["hstream_ui"]["stitching_calibration"]["artifacts_invalidated"] = true;
  stale_force_config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "stale-force";
  ok &= expect(
      hm::stitching::publish_game_config(superseded_force_dir, YAML::Dump(stale_force_config) + "\n").ok(),
      "stale forced configuration fixture must publish");
  hm::Configurator forced_configurator("superseded-force", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  ok &= expect(forced_configurator.configure().ok(), "forced configurator must load its stale snapshot");
  auto initially_current =
      hm::stitching::is_stitching_invalidation_cleanup_applied(superseded_force_dir.string(), "stale-force");
  ok &= expect(
      initially_current.ok() && initially_current.value(),
      "forced invalidation must initially pass the pre-clean revalidation");
  YAML::Node stale_offset_save = YAML::Clone(stale_force_config);
  stale_offset_save["game"]["stitching"]["frame_offsets"]["left"] = "9";
  stale_offset_save["game"]["stitching"]["frame_offsets"]["right"] = "0";
  stale_force_config["hstream_ui"]["stitching_calibration"]["artifacts_invalidated"] = false;
  stale_force_config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "newer-force";
  ok &= expect(
      hm::stitching::publish_game_config(superseded_force_dir, YAML::Dump(stale_force_config) + "\n").ok(),
      "newer forced invalidation fixture must publish");
  const absl::Status stale_save_status =
      forced_configurator.save_private_config(stale_offset_save, /*expected_invalidation_id=*/"stale-force");
  auto after_stale_save = hm::stitching::load_game_config_file(superseded_force_dir / "config.yaml");
  ok &= expect(
      stale_save_status.code() == absl::StatusCode::kAborted && after_stale_save.ok() &&
          after_stale_save->has_value() && !(**after_stale_save)["game"]["stitching"]["frame_offsets"] &&
          (**after_stale_save)["hstream_ui"]["stitching_calibration"]["invalidation_id"].as<std::string>() ==
              "newer-force",
      "A superseded forced save must not republish stale frame offsets after initial revalidation");
  const absl::Status non_forced_status = forced_configurator.complete_configuration(
      /*force=*/false,
      /*clean_stitching_artifacts=*/false,
      /*clean_stitching_from_control_points=*/false,
      /*clean_expected_invalidation_id=*/"stale-force");
  ok &= expect(
      non_forced_status.code() == absl::StatusCode::kAborted && fs::exists(superseded_force_dir / "seam_file.png"),
      "A non-forced input-stale run must abort before using a superseding artifact generation");
  const absl::Status forced_status = forced_configurator.complete_configuration(
      /*force=*/true,
      /*clean_stitching_artifacts=*/false,
      /*clean_stitching_from_control_points=*/false,
      /*clean_expected_invalidation_id=*/"stale-force");
  ok &= expect(
      forced_status.code() == absl::StatusCode::kAborted && fs::exists(superseded_force_dir / "seam_file.png"),
      "Forced configuration must abort before using or deleting a superseding artifact generation");

  const fs::path runtime_claim_dir = games / "runtime-claim";
  fs::create_directories(runtime_claim_dir);
  YAML::Node runtime_claim_config(YAML::NodeType::Map);
  runtime_claim_config["pipeline"]["application"]["complete-configuration"] = "1";
  runtime_claim_config["pipeline"]["hmstitcher"]["enable"] = "1";
  runtime_claim_config["pipeline"]["hmstitcher"]["one-pass-mode"] = "1";
  runtime_claim_config["stitching"]["mapping_backend"] = "nona";
  runtime_claim_config["stitching"]["projection"] = "general-panini";
  runtime_claim_config["stitching"]["run_autooptimizer"] = true;
  runtime_claim_config["stitching"]["projection_parameters"]["general-panini"] = YAML::Load("[120, 15, -20]");
  runtime_claim_config["hstream_ui"]["stitching_calibration"]["control_points"] = 1500;
  runtime_claim_config["hstream_ui"]["stitching_calibration"]["frame_count"] = 4;
  runtime_claim_config["hstream_ui"]["stitching_calibration"]["status"] = "complete";
  runtime_claim_config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "runtime-claim-a";
  ok &= expect(
      hm::stitching::publish_game_config(runtime_claim_dir, YAML::Dump(runtime_claim_config) + "\n").ok(),
      "runtime-discovered calibration fixture must publish");
  hm::Configurator runtime_claim_configurator(
      "runtime-claim", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  hm::Configurator runtime_claim_peer("runtime-claim", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  ok &= expect(
      runtime_claim_configurator.configure().ok() && runtime_claim_peer.configure().ok(),
      "runtime-discovered calibration contexts must load the same completed owner");
  const absl::Status runtime_claim_status = runtime_claim_configurator.complete_configuration(
      /*force=*/false,
      /*clean_stitching_artifacts=*/false,
      /*clean_stitching_from_control_points=*/false,
      /*clean_expected_invalidation_id=*/"runtime-claim-a");
  const absl::Status runtime_peer_claim_status = runtime_claim_peer.complete_configuration(
      /*force=*/false,
      /*clean_stitching_artifacts=*/false,
      /*clean_stitching_from_control_points=*/false,
      /*clean_expected_invalidation_id=*/"runtime-claim-a");
  const YAML::Node claimed_runtime_config = YAML::LoadFile((runtime_claim_dir / "config.yaml").string());
  const YAML::Node claimed_runtime_calibration = claimed_runtime_config["hstream_ui"]["stitching_calibration"];
  ok &= expect(
      runtime_claim_status.code() != absl::StatusCode::kAborted &&
          runtime_claim_configurator.stitching_calibration_required() &&
          runtime_claim_configurator.active_stitching_invalidation_id() == "runtime-claim-a" &&
          runtime_peer_claim_status.code() != absl::StatusCode::kAborted &&
          runtime_claim_peer.active_stitching_invalidation_id() == "runtime-claim-a" &&
          claimed_runtime_calibration["status"].as<std::string>() == "pending" &&
          claimed_runtime_calibration["stale_from"].as<std::string>() == "input" &&
          claimed_runtime_calibration["artifacts_invalidated"].as<bool>() &&
          claimed_runtime_calibration["backend_generation"]["projection_parameters"][0].as<double>() == 120.0 &&
          claimed_runtime_calibration["backend_generation"]["projection_parameters"][1].as<double>() == 15.0 &&
          claimed_runtime_calibration["backend_generation"]["projection_parameters"][2].as<double>() == -20.0 &&
          claimed_runtime_calibration["invalidation_id"].as<std::string>() == "runtime-claim-a",
      "runtime-discovered missing mappings with custom projection parameters must share the reserved owner before "
      "Hugin publication can begin");

  const fs::path superseded_complete_dir = games / "superseded-complete";
  fs::create_directories(superseded_complete_dir);
  YAML::Node reserved_complete_config(YAML::NodeType::Map);
  reserved_complete_config["pipeline"]["application"]["complete-configuration"] = "1";
  reserved_complete_config["pipeline"]["hmstitcher"]["enable"] = "1";
  reserved_complete_config["hstream_ui"]["stitching_calibration"]["status"] = "complete";
  reserved_complete_config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "reserved-complete-a";
  ok &= expect(
      hm::stitching::publish_game_config(superseded_complete_dir, YAML::Dump(reserved_complete_config) + "\n").ok(),
      "reserved complete owner fixture must publish");
  hm::Configurator complete_configurator(
      "superseded-complete", baseline_root.string(), hm::Configurator::kUseConfigFileGpu);
  ok &= expect(complete_configurator.configure().ok(), "reserved complete configurator must load its owner snapshot");
  reserved_complete_config["hstream_ui"]["stitching_calibration"]["status"] = "pending";
  reserved_complete_config["hstream_ui"]["stitching_calibration"]["stale_from"] = "input";
  reserved_complete_config["hstream_ui"]["stitching_calibration"]["artifacts_invalidated"] = false;
  reserved_complete_config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "reserved-complete-b";
  ok &= expect(
      hm::stitching::publish_game_config(superseded_complete_dir, YAML::Dump(reserved_complete_config) + "\n").ok(),
      "newer owner must supersede a reserved complete run after load_config");
  const absl::Status superseded_complete_status = complete_configurator.complete_configuration(
      /*force=*/false,
      /*clean_stitching_artifacts=*/false,
      /*clean_stitching_from_control_points=*/false,
      /*clean_expected_invalidation_id=*/"reserved-complete-a");
  ok &= expect(
      superseded_complete_status.code() == absl::StatusCode::kAborted,
      "A reserved complete owner superseded after load_config must abort at the final launch boundary");

  fs::remove_all(root);
  if (original_home.empty())
    ::unsetenv("HOME");
  else
    ::setenv("HOME", original_home.c_str(), 1);
  if (original_game_root.empty())
    ::unsetenv("HM_GAME_DIR");
  else
    ::setenv("HM_GAME_DIR", original_game_root.c_str(), 1);
  if (original_output_root.empty())
    ::unsetenv("HM_OUTPUT_WORK_DIR");
  else
    ::setenv("HM_OUTPUT_WORK_DIR", original_output_root.c_str(), 1);
  if (original_xdg_runtime_dir.empty())
    ::unsetenv("XDG_RUNTIME_DIR");
  else
    ::setenv("XDG_RUNTIME_DIR", original_xdg_runtime_dir.c_str(), 1);
  return ok ? 0 : 1;
}
