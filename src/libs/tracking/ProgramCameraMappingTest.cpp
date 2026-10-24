#include "hstream/src/libs/tracking/ProgramCameraMapping.h"
#include "hstream/src/libs/common/BaselineConfig.h"

#include <filesystem>
#include <iostream>

namespace {
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 2)
    return 2;
  bool ok = true;
  const auto bundled_baseline = hm::baseline_config::load_from_root(std::filesystem::path(argv[1]).parent_path());
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
      ? hm::tracking::ResolveProgramCameraConfig(bundled_baseline->values, {}, /*native_base_rank=*/0, playtracker_base)
      : absl::StatusOr<YAML::Node>(bundled_baseline.status());
  if (effective_playtracker.ok()) {
    const YAML::Node play_tracker = (*effective_playtracker)["play-tracker"];
    const YAML::Node fast = play_tracker["live-boxes"][0];
    const YAML::Node follower = play_tracker["live-boxes"][1];
    ok &= expect(
        play_tracker["preserve-me"].as<std::string>() == "yes" && fast["preserve-fast"].as<int>() == 1 &&
            follower["preserve-follower"].as<int>() == 1 && play_tracker["camera-name"].as<std::string>() == "GoPro" &&
            play_tracker["no-wide-start"].as<bool>() && play_tracker["ignore-largest-bbox"].as<bool>() &&
            play_tracker["ignore-largest-bbox-count"].as<int>() == 1 &&
            !play_tracker["ignore-oversized-bboxes"].as<bool>() &&
            play_tracker["oversized-bbox-percent"].as<double>() == 100.0 &&
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
    const hm::tracking::ConfigLeafRanks explicit_overrides = {
        {"rink.camera.stop_on_dir_change_delay", 1},
        {"rink.camera.zoom_in_aggressiveness", 1},
        {"rink.camera.breakaway_detection.overshoot_stop_delay_count", 1},
    };
    const auto overlaid_playtracker = hm::tracking::ResolveProgramCameraConfig(
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
    const auto custom_only =
        hm::tracking::ResolveProgramCameraConfig(bundled_baseline->values, {}, /*native_base_rank=*/0, custom_base);
    YAML::Node different_effective = YAML::Clone(bundled_baseline->values);
    different_effective["rink"]["camera"]["sticky_translation_gaussian_mult"] = 7.25;
    const hm::tracking::ConfigLeafRanks different_explicit = {{"rink.camera.sticky_translation_gaussian_mult", 1}};
    const auto explicit_different = hm::tracking::ResolveProgramCameraConfig(
        different_effective, different_explicit, /*native_base_rank=*/0, custom_base);
    const hm::tracking::ConfigLeafRanks equal_explicit = {{"rink.camera.sticky_translation_gaussian_mult", 1}};
    const auto explicit_equal = hm::tracking::ResolveProgramCameraConfig(
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
    const auto reordered =
        hm::tracking::ResolveProgramCameraConfig(bundled_baseline->values, {}, /*native_base_rank=*/0, reordered_base);
    YAML::Node one_box = YAML::Clone(playtracker_base);
    YAML::Node one_box_sequence(YAML::NodeType::Sequence);
    YAML::Node operator_box(YAML::NodeType::Map);
    operator_box["name"] = "operator_only";
    one_box_sequence.push_back(operator_box);
    one_box["play-tracker"]["live-boxes"] = one_box_sequence;
    const auto one_box_effective =
        hm::tracking::ResolveProgramCameraConfig(bundled_baseline->values, {}, /*native_base_rank=*/0, one_box);
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
    const hm::tracking::ConfigLeafRanks malformed_explicit = {{"play_tracker.no_wide_start", 1}};
    const auto malformed_canonical = hm::tracking::ResolveProgramCameraConfig(
        malformed_effective, malformed_explicit, /*native_base_rank=*/0, playtracker_base);
    YAML::Node malformed_native = YAML::Clone(playtracker_base);
    malformed_native["play-tracker"]["no-wide-start"] = "not-a-boolean";
    const auto malformed_custom = hm::tracking::ResolveProgramCameraConfig(
        bundled_baseline->values, {}, /*native_base_rank=*/0, malformed_native);
    YAML::Node invalid_zoom = YAML::Clone(bundled_baseline->values);
    invalid_zoom["rink"]["camera"]["zoom_in_aggressiveness"] = 101;
    const hm::tracking::ConfigLeafRanks invalid_zoom_explicit = {{"rink.camera.zoom_in_aggressiveness", 1}};
    const auto invalid_zoom_config = hm::tracking::ResolveProgramCameraConfig(
        invalid_zoom, invalid_zoom_explicit, /*native_base_rank=*/0, playtracker_base);
    YAML::Node negative_zoom = YAML::Clone(bundled_baseline->values);
    negative_zoom["rink"]["camera"]["zoom_in_aggressiveness"] = -1;
    const auto negative_zoom_config = hm::tracking::ResolveProgramCameraConfig(
        negative_zoom, invalid_zoom_explicit, /*native_base_rank=*/0, playtracker_base);
    YAML::Node minimum_zoom = YAML::Clone(bundled_baseline->values);
    minimum_zoom["rink"]["camera"]["zoom_in_aggressiveness"] = 0;
    const auto minimum_zoom_config = hm::tracking::ResolveProgramCameraConfig(
        minimum_zoom, invalid_zoom_explicit, /*native_base_rank=*/0, playtracker_base);
    YAML::Node maximum_zoom = YAML::Clone(bundled_baseline->values);
    maximum_zoom["rink"]["camera"]["zoom_in_aggressiveness"] = 100;
    const auto maximum_zoom_config = hm::tracking::ResolveProgramCameraConfig(
        maximum_zoom, invalid_zoom_explicit, /*native_base_rank=*/0, playtracker_base);
    ok &= expect(
        !malformed_canonical.ok() && !malformed_custom.ok() && absl::IsInvalidArgument(invalid_zoom_config.status()) &&
            absl::IsInvalidArgument(negative_zoom_config.status()) && minimum_zoom_config.ok() &&
            maximum_zoom_config.ok() &&
            (*minimum_zoom_config)["play-tracker"]["zoom-in-aggressiveness"].as<int>() == 0 &&
            (*maximum_zoom_config)["play-tracker"]["zoom-in-aggressiveness"].as<int>() == 100,
        "Zoom aggressiveness must accept both boundaries and reject values below zero or above one hundred");
  }

  return ok ? 0 : 1;
}
