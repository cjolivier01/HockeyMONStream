#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerCtx.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <unistd.h>

namespace {

bool near(float actual, float expected) {
  return std::abs(actual - expected) < 0.0001f;
}

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

} // namespace

int main() {
  const YAML::Node yaml = YAML::Load(R"(
camera-name: GoPro
no-wide-start: false
ignore-largest-bbox: false
max-speed-ratio-x: 2.0
max-speed-ratio-y: 3.0
max-accel-ratio-x: 4.0
max-accel-ratio-y: 5.0
follower-box-min-height-ratio: 0.3
zoom-in-aggressiveness: 25
min-considered-group-velocity: 3.0
group-ratio-threshold: 0.5
group-velocity-speed-ratio: 0.3
scale-speed-constraints: 3.0
nonstop-delay-count: 2
overshoot-scale-speed-ratio: 0.7
overshoot-stop-delay-count: 6
live-boxes:
  - name: current_roi
    time-to-dest-speed-limit-frames: 20
    time-to-dest-stop-speed-threshold: 0.25
    resizing-stop-on-dir-change-delay: 4
    resizing-cancel-stop-on-opposite-dir: true
    resizing-stop-cancel-hysteresis-frames: 10
    resizing-stop-delay-cooldown-frames: 2
    resizing-time-to-dest-speed-limit-frames: 10
    resizing-time-to-dest-stop-speed-threshold: 0.35
  - name: current_roi_aspect
    stop-translation-on-dir-change-delay: 10
    cancel-stop-on-opposite-dir: true
    cancel-stop-hysteresis-frames: 2
    stop-delay-cooldown-frames: 2
    post-nonstop-stop-delay-count: 6
    time-to-dest-speed-limit-frames: 20
    time-to-dest-stop-speed-threshold: 0.25
    resizing-stop-on-dir-change-delay: 4
    resizing-cancel-stop-on-opposite-dir: true
    resizing-stop-cancel-hysteresis-frames: 10
    resizing-stop-delay-cooldown-frames: 2
    resizing-time-to-dest-speed-limit-frames: 10
    resizing-time-to-dest-stop-speed-threshold: 0.35
    sticky-size-ratio-to-frame-width: 10.0
    sticky-translation-gaussian-mult: 5.0
    unsticky-translation-size-ratio: 0.75
    scale-dest-width: 1.45
    scale-dest-height: 1.45
)");
  const hm::play_tracker::PlayTrackerConfig config =
      gst_hm_playtracker::create_play_tracker_config(hm::BBox(0, 0, 2000, 1000), yaml);
  if (!expect(config.living_boxes.size() == 2, "Expected fast and follower boxes"))
    return 1;
  const auto& fast = config.living_boxes[0];
  const auto& follower = config.living_boxes[1];
  bool ok = true;
  ok &= expect(!config.no_wide_start && !config.ignore_largest_bbox, "Global baseline booleans should be honored");
  ok &= expect(config.play_detector.overshoot_stop_delay_count == 6, "Breakaway braking should come from YAML");
  ok &= expect(
      near(fast.max_speed_x, 36.0f) && near(fast.max_speed_y, 54.0f) && near(fast.max_accel_x, 4.4f) &&
          near(fast.max_accel_y, 5.5f) && near(fast.max_speed_w, 20.0f) && near(fast.max_speed_h, 30.0f) &&
          near(fast.max_accel_w, 4.4f) && near(fast.max_accel_h, 5.5f) && near(follower.max_speed_x, 24.0f) &&
          near(follower.max_speed_y, 36.0f) && near(follower.max_accel_x, 4.0f) && near(follower.max_accel_y, 5.0f) &&
          near(follower.max_speed_w, 13.333333f) && near(follower.max_speed_h, 20.0f) &&
          near(follower.max_accel_w, 4.0f) && near(follower.max_accel_h, 5.0f),
      "Baseline speed and acceleration ratios should scale native arena-derived constraints");
  ok &= expect(near(follower.min_height, 300.0f), "Follower minimum height ratio should use the arena height");
  ok &= expect(
      near(follower.size_ratio_thresh_shrink_dw, 0.08f) && near(follower.size_ratio_thresh_shrink_dh, 0.10f),
      "Default zoom-in aggressiveness must preserve the native sticky shrink thresholds exactly");

  YAML::Node eager_zoom_yaml = YAML::Clone(yaml);
  eager_zoom_yaml["zoom-in-aggressiveness"] = 100;
  const auto eager_zoom_config =
      gst_hm_playtracker::create_play_tracker_config(hm::BBox(0, 0, 2000, 1000), eager_zoom_yaml);
  ok &= expect(
      near(eager_zoom_config.living_boxes.back().size_ratio_thresh_shrink_dw, 0.008f) &&
          near(eager_zoom_config.living_boxes.back().size_ratio_thresh_shrink_dh, 0.010f),
      "Maximum zoom-in aggressiveness should lower follower shrink hysteresis to one tenth");
  YAML::Node reluctant_zoom_yaml = YAML::Clone(yaml);
  reluctant_zoom_yaml["zoom-in-aggressiveness"] = 0;
  const auto reluctant_zoom_config =
      gst_hm_playtracker::create_play_tracker_config(hm::BBox(0, 0, 2000, 1000), reluctant_zoom_yaml);
  ok &= expect(
      near(reluctant_zoom_config.living_boxes.back().size_ratio_thresh_shrink_dw, 0.16f) &&
          near(reluctant_zoom_config.living_boxes.back().size_ratio_thresh_shrink_dh, 0.20f),
      "Minimum zoom-in aggressiveness should double follower shrink hysteresis");
  ok &= expect(
      fast.time_to_dest_speed_limit_frames == 20 && near(fast.time_to_dest_stop_speed_threshold, 0.25f) &&
          fast.resizing_stop_on_dir_change_delay == 4 && fast.resizing_cancel_stop_on_opposite_dir &&
          fast.resizing_stop_cancel_hysteresis_frames == 10 && fast.resizing_stop_delay_cooldown_frames == 2 &&
          fast.resizing_time_to_dest_speed_limit_frames == 10 &&
          near(fast.resizing_time_to_dest_stop_speed_threshold, 0.35f),
      "Fast-box timing and stop thresholds should be parsed from the materialized baseline");
  ok &= expect(
      follower.stop_translation_on_dir_change_delay == 10 && follower.cancel_stop_on_opposite_dir &&
          follower.cancel_stop_hysteresis_frames == 2 && follower.stop_delay_cooldown_frames == 2 &&
          follower.post_nonstop_stop_delay_count == 6,
      "Follower braking defaults should be parsed from the materialized baseline");

  YAML::Node reordered_yaml = YAML::Clone(yaml);
  YAML::Node reordered_boxes(YAML::NodeType::Sequence);
  YAML::Node reordered_fast = YAML::Clone(yaml["live-boxes"][0]);
  reordered_fast["stop-translation-on-dir-change-delay"] = 11;
  YAML::Node reordered_follower = YAML::Clone(yaml["live-boxes"][1]);
  reordered_follower["stop-translation-on-dir-change-delay"] = 22;
  YAML::Node additional_box = YAML::Clone(yaml["live-boxes"][0]);
  additional_box["name"] = "operator_extra";
  additional_box["stop-translation-on-dir-change-delay"] = 33;
  reordered_boxes.push_back(reordered_follower);
  reordered_boxes.push_back(additional_box);
  reordered_boxes.push_back(reordered_fast);
  reordered_yaml["live-boxes"] = reordered_boxes;
  const hm::play_tracker::PlayTrackerConfig reordered_config =
      gst_hm_playtracker::create_play_tracker_config(hm::BBox(0, 0, 2000, 1000), reordered_yaml);
  ok &= expect(
      reordered_config.living_boxes.size() == 3 &&
          reordered_config.living_boxes[0].stop_translation_on_dir_change_delay == 11 &&
          reordered_config.living_boxes[1].stop_translation_on_dir_change_delay == 33 &&
          reordered_config.living_boxes[2].stop_translation_on_dir_change_delay == 22 &&
          near(reordered_config.living_boxes[2].min_height, 300.0f),
      "Native tracker construction must normalize fast first and follower last while retaining additional boxes");

  NvDsBatchMeta* draw_batch = nvds_create_batch_meta(1);
  NvDsFrameMeta* draw_frame_meta = draw_batch ? nvds_acquire_frame_meta_from_pool(draw_batch) : nullptr;
  if (!expect(draw_batch && draw_frame_meta, "Expected DeepStream metadata for arbitrary-box draw test")) {
    if (draw_batch)
      nvds_destroy_batch_meta(draw_batch);
    return 1;
  }
  draw_frame_meta->source_id = 0;
  draw_frame_meta->source_frame_width = 2000;
  draw_frame_meta->source_frame_height = 1000;
  nvds_add_frame_meta_to_batch(draw_batch, draw_frame_meta);
  NvBufSurfaceParams draw_surface{};
  draw_surface.width = 2000;
  draw_surface.height = 1000;
  DsPlayTrackerCtx draw_context;
  draw_context.arena_box = hm::BBox(0, 0, 2000, 1000);
  auto& draw_tracker = draw_context.play_trackers[0];
  draw_tracker.base_play_tracker_config = reordered_config;
  draw_tracker.play_tracker_config = reordered_config;
  draw_tracker.play_tracker = std::make_unique<hm::play_tracker::PlayTracker>(draw_context.arena_box, reordered_config);
  GstDsPlayTrackerFrame draw_frame;
  draw_frame.frame_meta = draw_frame_meta;
  draw_frame.input_surf_params = &draw_surface;
  draw_frame.play_tracker_results.tracking_boxes = {
      hm::BBox(100, 100, 300, 300), hm::BBox(400, 100, 600, 300), hm::BBox(700, 100, 900, 300)};
  const absl::Status draw_status = DsPlayTrackerDrawToDisplayMeta(&draw_context, draw_frame);
  ok &= expect(draw_status.ok(), "Display drawing must support more than two live boxes");
  DsPlayTrackerRuntimeTuning zoom_tuning;
  zoom_tuning.apply_to_fast_box = false;
  zoom_tuning.apply_to_follower_box = false;
  zoom_tuning.update_motion_tuning = false;
  zoom_tuning.zoom_in_aggressiveness = 100;
  const absl::Status zoom_status = DsPlayTrackerCtxApplyRuntimeTuning(&draw_context, zoom_tuning);
  ok &= expect(
      zoom_status.ok() &&
          near(draw_tracker.play_tracker_config.living_boxes.back().size_ratio_thresh_shrink_dw, 0.008f) &&
          near(draw_tracker.play_tracker_config.living_boxes.back().size_ratio_thresh_shrink_dh, 0.010f) &&
          near(draw_tracker.play_tracker_config.living_boxes.front().size_ratio_thresh_shrink_dw, 0.08f),
      "Live zoom tuning must update only the follower shrink decision without recreating the tracker");
  nvds_destroy_batch_meta(draw_batch);

  YAML::Node one_box_yaml = YAML::Clone(yaml);
  YAML::Node one_box_sequence(YAML::NodeType::Sequence);
  YAML::Node one_box = YAML::Clone(yaml["live-boxes"][1]);
  one_box["name"] = "operator_only";
  one_box["stop-translation-on-dir-change-delay"] = 44;
  one_box_sequence.push_back(one_box);
  one_box_yaml["live-boxes"] = one_box_sequence;
  const hm::play_tracker::PlayTrackerConfig one_box_config =
      gst_hm_playtracker::create_play_tracker_config(hm::BBox(0, 0, 2000, 1000), one_box_yaml);
  ok &= expect(
      one_box_config.living_boxes.size() == 1 &&
          one_box_config.living_boxes[0].stop_translation_on_dir_change_delay == 44 &&
          near(one_box_config.living_boxes[0].min_height, 300.0f),
      "A native one-box tracker config must retain one box and use it for both fast and follower roles");

  const std::filesystem::path validation_path = std::filesystem::temp_directory_path() /
      ("playtracker-baseline-validation-" + std::to_string(::getpid()) + ".yaml");
  YAML::Node document(YAML::NodeType::Map);
  document["play-tracker"] = YAML::Clone(yaml);
  std::ofstream(validation_path) << YAML::Dump(document) << '\n';
  const absl::Status valid_status = DsPlayTrackerValidateConfigFile(validation_path.string());
  document["play-tracker"] = YAML::Clone(one_box_yaml);
  std::ofstream(validation_path) << YAML::Dump(document) << '\n';
  const absl::Status one_box_status = DsPlayTrackerValidateConfigFile(validation_path.string());
  document["play-tracker"] = YAML::Clone(yaml);
  document["play-tracker"]["no-wide-start"] = "not-a-boolean";
  std::ofstream(validation_path) << YAML::Dump(document) << '\n';
  const absl::Status malformed_status = DsPlayTrackerValidateConfigFile(validation_path.string());
  document["play-tracker"] = YAML::Clone(yaml);
  document["play-tracker"]["zoom-in-aggressiveness"] = 101;
  std::ofstream(validation_path) << YAML::Dump(document) << '\n';
  const absl::Status invalid_zoom_status = DsPlayTrackerValidateConfigFile(validation_path.string());
  std::filesystem::remove(validation_path);
  ok &= expect(
      valid_status.ok() && one_box_status.ok() && !malformed_status.ok() && !invalid_zoom_status.ok(),
      "Native validation must accept one-box compatibility and type-check every baseline-backed tracker field");
  return ok ? 0 : 1;
}
