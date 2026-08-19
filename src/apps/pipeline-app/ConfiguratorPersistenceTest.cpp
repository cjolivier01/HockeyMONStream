#include "src/apps/pipeline-app/StitcherOnePassConfig.h"
#include "src/apps/pipeline-app/configurator.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

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
    overlaid["rink"]["camera"]["breakaway_detection"]["overshoot_stop_delay_count"] = 12;
    const hm::configurator_internal::ConfigLeafRanks explicit_overrides = {
        {"rink.camera.stop_on_dir_change_delay", 1},
        {"rink.camera.breakaway_detection.overshoot_stop_delay_count", 1},
    };
    const auto overlaid_playtracker = hm::configurator_internal::build_effective_playtracker_config(
        overlaid, explicit_overrides, /*native_base_rank=*/0, playtracker_base);
    ok &= expect(
        overlaid_playtracker.ok() &&
            (*overlaid_playtracker)["play-tracker"]["overshoot-stop-delay-count"].as<int>() == 12 &&
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
    ok &= expect(
        !malformed_canonical.ok() && !malformed_custom.ok(),
        "Malformed baseline-backed canonical and custom-native scalar types must fail materialization");
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

  const fs::path structural_custom_path = root / "mapping-structural-custom.yaml";
  YAML::Node structural_custom = YAML::Clone(mapping_structure);
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

  const fs::path canonical_game_dir = games / "mapping-canonical";
  fs::create_directories(canonical_game_dir);
  YAML::Node canonical_overrides(YAML::NodeType::Map);
  canonical_overrides["stitching"]["enabled"] = false;
  canonical_overrides["stitching"]["minimize_blend"] = true;
  canonical_overrides["stitching"]["dtype"] = "float16";
  canonical_overrides["stitching"]["post_stitch_rotate_degrees"] = 15.0;
  canonical_overrides["rink"]["camera"]["crop_image"] = false;
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
          mapped_canonical["hmstitcher"]["minimize-blend"].as<int>() == 1 &&
          mapped_canonical["hmstitcher"]["stitch-compute-precision"].as<std::string>() == "fp16" &&
          mapped_canonical["hmstitcher"]["post-stitch-rotate-degrees"].as<double>() == 15.0 &&
          mapped_canonical["hmplaycropper"]["no-crop"].as<int>() == 1 &&
          mapped_canonical["hmplaycropper"]["plot-play-tracking"].as<int>() == 1 &&
          mapped_canonical["hmplaycropper"]["plot-player-tracking"].as<int>() == 1 &&
          mapped_canonical["ds-playtracker"]["draw"].as<int>() == 0 &&
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

  ok &= expect(
      mapping_canonical.apply_config_item("pipeline.hmstitcher.enable", "1").ok() &&
          mapping_canonical.apply_supported_baseline_mappings().ok() &&
          mapping_canonical.config()["pipeline"]["hmstitcher"]["enable"].as<int>() == 1 &&
          mapping_canonical.apply_config_item("stitching.enabled", "false").ok() &&
          mapping_canonical.apply_supported_baseline_mappings().ok() &&
          mapping_canonical.config()["pipeline"]["hmstitcher"]["enable"].as<int>() == 1,
      "A higher-ranked direct native value must win, and direct native must win a same-rank canonical tie");

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
          !hm::get_node(layered.game_private_config(), "stitching.stitch_frame_time").has_value(),
      "Config precedence must be baseline, then user overlay, then game-private YAML");
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
  std::ofstream(stale_versioned_run, std::ios::binary) << "stale versioned run with live UI parent";
  std::ofstream(stale_pre_version_run, std::ios::binary) << "stale pre-version run with live UI parent";
  std::ofstream(dead_versioned_run, std::ios::binary) << "stale versioned run with dead backend and UI";
  std::ofstream(dead_pre_version_run, std::ios::binary) << "stale pre-version run with dead backend and UI";
  std::ofstream(live_run, std::ios::binary) << "live unique run data";
  std::ofstream(legacy_live_ui_run, std::ios::binary) << "legacy work owned by live UI";
  const auto stale_recoveries = hm::configurator_internal::recover_stale_archive_work_files(custom_archive);
  ok &= expect(
      stale_recoveries.ok() && stale_recoveries->size() == 3 && !fs::exists(stale_run) &&
          fs::is_regular_file(stale_versioned_run) && fs::is_regular_file(stale_pre_version_run) &&
          !fs::exists(dead_versioned_run) && !fs::exists(dead_pre_version_run) &&
          std::all_of(
              stale_recoveries->begin(),
              stale_recoveries->end(),
              [](const fs::path& path) { return fs::is_regular_file(path); }) &&
          fs::is_regular_file(live_run) && fs::is_regular_file(legacy_live_ui_run),
      "Restart recovery must retain pre-v3 work while either its backend or UI owner is alive");
  const auto repeated_stale_recoveries = hm::configurator_internal::recover_stale_archive_work_files(custom_archive);
  ok &= expect(
      repeated_stale_recoveries.ok() && repeated_stale_recoveries->empty() && stale_recoveries.ok() &&
          fs::is_regular_file(stale_recoveries->front()),
      "Restart recovery must leave an already-recovered unique run at its stable recovery path");

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
