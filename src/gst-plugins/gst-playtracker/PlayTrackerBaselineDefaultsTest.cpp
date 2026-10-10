#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerCtx.h"
#include "hstream/src/libs/common/PreviewOverlayMeta.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>

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

bool verify_player_exclusions(const YAML::Node& yaml) {
  namespace overlay = hm::preview_overlay;
  const hm::BBox arena(0, 0, 2000, 1000);
  auto config = gst_hm_playtracker::create_play_tracker_config(arena, yaml);
  config.ignore_largest_bbox = true;
  config.ignore_largest_bbox_count = 1;
  config.ignore_outlier_players = false;
  DsPlayTrackerCtx context;
  context.arena_box = arena;
  auto& tracker = context.play_trackers[0];
  tracker.play_tracker_config = config;
  tracker.base_play_tracker_config = config;
  tracker.play_tracker = std::make_unique<hm::play_tracker::PlayTracker>(arena, config);
  DsPlayTrackerCtxSetPreviewOverlayFlags(&context, kPreviewOverlayPlayers);
  bool ok = true;
  // Exercise the real DeepStream adapter and pinned native tracker, including
  // live tuning on the same tracker. Tracker coordinates are half metadata size.
  struct Case {
    int players, count;
    bool oversized, drawing;
    int expected;
  };
  for (const auto test :
       {Case{5, 1, false, true, 1},
        Case{5, 2, false, true, 2},
        Case{5, 0, false, true, 0},
        Case{5, 0, true, true, 1},
        Case{3, 2, true, true, 0},
        Case{5, 1, false, false, 1}}) {
    DsPlayTrackerRuntimeTuning tuning;
    tuning.ignore_largest_bbox_count = test.count;
    tuning.ignore_oversized_bboxes = test.oversized;
    tuning.oversized_bbox_percent = 100;
    ok &= expect(DsPlayTrackerCtxApplyRuntimeTuning(&context, tuning).ok(), "Apply live exclusion settings");
    DsPlayTrackerCtxSetPreviewOverlayFlags(&context, test.drawing ? kPreviewOverlayPlayers : 0);
    auto* batch = nvds_create_batch_meta(1);
    auto* frame = batch ? nvds_acquire_frame_meta_from_pool(batch) : nullptr;
    if (!expect(frame != nullptr, "Allocate exclusion fixture"))
      return false;
    nvds_add_frame_meta_to_batch(batch, frame);
    frame->source_frame_width = 2000;
    frame->source_frame_height = 1000;
    std::vector<NvDsObjectMeta*> objects;
    for (int i = 0; i < test.players; ++i) {
      auto* object = nvds_acquire_obj_meta_from_pool(batch);
      object->class_id = 0;
      object->object_id = (uint64_t{1} << 40) + i;
      object->tracker_bbox_info.org_bbox_coords = {
          i == 0 ? 50.0F : 500.0F + i * 25, 100.0F, i == 0 ? 200.0F : 20.0F, i == 0 ? 200.0F : 40.0F};
      const auto& box = object->tracker_bbox_info.org_bbox_coords;
      object->rect_params.left = box.left * 2;
      object->rect_params.top = box.top * 2;
      object->rect_params.width = box.width * 2;
      object->rect_params.height = box.height * 2;
      // A stale flag must clear when this frame no longer excludes that ID.
      overlay::set_player_ignored(*object, test.drawing);
      nvds_add_obj_meta_to_frame(frame, object, nullptr);
      objects.push_back(object);
    }
    NvBufSurfaceParams surface{};
    surface.width = 1000;
    surface.height = 500;
    GstDsPlayTrackerFrame input;
    input.frame_meta = frame;
    input.input_surf_params = &surface;
    ok &= expect(DsPlayTrackerProcessFrame(&context, input, nullptr), "Process native exclusion frame");
    const auto& results = input.play_tracker_results;
    ok &= expect(
        results.size_ignored_tracking_boxes.size() == static_cast<size_t>(test.expected),
        "Native count/oversized/minimum-three filtering differs");
    if (test.expected) {
      ok &= expect(
          results.size_ignored_tracking_boxes[0].tracking_id == objects[0]->object_id,
          "Native tracker did not exclude the largest area");
      ok &= expect(
          results.final_cluster_box.left >= 1000, "Excluded foreground player still enlarged the camera cluster");
    }
    const auto* snapshot = overlay::find_overlay_snapshot_meta(frame);
    if (test.drawing) {
      ok &= expect(snapshot && snapshot->player_rects.size() == objects.size(), "Snapshot retains all player boxes");
      int marked = 0, saved = 0;
      for (auto* object : objects)
        marked += overlay::player_is_ignored(*object);
      if (snapshot)
        for (const auto& player : snapshot->player_rects)
          saved += player.ignored;
      ok &= expect(marked == test.expected && saved == test.expected, "Ignored metadata must match native results");
      if (test.expected)
        ok &= expect(overlay::player_is_ignored(*objects[0]), "Largest full-width track ID was not marked");
    } else {
      ok &= expect(
          !snapshot && !overlay::player_is_ignored(*objects[0]),
          "Drawing disabled should filter without producing overlay metadata");
    }
    nvds_destroy_batch_meta(batch);
  }
  return ok;
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
  ok &= verify_player_exclusions(yaml);
  ok &= expect(!config.no_wide_start && !config.ignore_largest_bbox, "Global baseline booleans should be honored");
  for (const char* key : {"no-wide-start", "no_wide_start"}) {
    for (const auto& [value, expected] : {std::pair{"true", true}, {"false", false}, {"1", true}, {"0", false}}) {
      YAML::Node bool_yaml = YAML::Clone(yaml);
      bool_yaml.remove("no-wide-start");
      bool_yaml[key] = YAML::Load(value);
      std::ostringstream diagnostics;
      auto* saved_stderr = std::cerr.rdbuf(diagnostics.rdbuf());
      const auto bool_config = gst_hm_playtracker::create_play_tracker_config(hm::BBox(0, 0, 2000, 1000), bool_yaml);
      std::cerr.rdbuf(saved_stderr);
      ok &= expect(bool_config.no_wide_start == expected, "no-wide-start must honor YAML booleans and legacy 0/1");
      // A rejected false value would otherwise pass by retaining the default.
      ok &= expect(diagnostics.str().empty(), "Valid no-wide-start values must parse without diagnostics");
    }
  }
  ok &= expect(
      config.ignore_largest_bbox_count == 1 && !config.ignore_oversized_bboxes &&
          config.oversized_bbox_percent == 100.0,
      "Legacy configs retain the default count and disabled percentage filter");
  YAML::Node size_yaml = YAML::Clone(yaml);
  size_yaml["ignore-largest-bbox-count"] = 3;
  size_yaml["ignore-oversized-bboxes"] = true;
  size_yaml["oversized-bbox-percent"] = 150;
  const auto size_config = gst_hm_playtracker::create_play_tracker_config(hm::BBox(0, 0, 2000, 1000), size_yaml);
  ok &= expect(
      size_config.ignore_largest_bbox_count == 3 && size_config.ignore_oversized_bboxes &&
          size_config.oversized_bbox_percent == 150,
      "Native YAML player size settings are parsed");
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
  DsPlayTrackerCtxSetDraw(&draw_context, true);
  ok &= expect(
      draw_context.draw.load(std::memory_order_relaxed),
      "Play-tracker display metadata must be live-toggleable on an active context");
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
  DsPlayTrackerCtxSetPreviewOverlayFlags(&draw_context, kPreviewOverlayPlayers);
  ok &= expect(
      draw_context.draw.load(std::memory_order_relaxed),
      "Enabling preview metadata must preserve a configured production draw=true state");
  NvDsBatchMeta* preview_batch = nvds_create_batch_meta(1);
  NvDsFrameMeta* preview_frame_meta = preview_batch ? nvds_acquire_frame_meta_from_pool(preview_batch) : nullptr;
  ok &= expect(preview_batch && preview_frame_meta, "Expected DeepStream metadata for preview-only draw test");
  if (preview_frame_meta) {
    preview_frame_meta->source_id = 0;
    preview_frame_meta->source_frame_width = 2000;
    preview_frame_meta->source_frame_height = 1000;
    nvds_add_frame_meta_to_batch(preview_batch, preview_frame_meta);
    GstDsPlayTrackerFrame preview_frame = draw_frame;
    preview_frame.frame_meta = preview_frame_meta;
    DsPlayTrackerCtxSetDraw(&draw_context, false);
    DsPlayTrackerCtxSetPreviewOverlayFlags(&draw_context, 0);
    ok &= expect(
        DsPlayTrackerAttachPreviewSnapshot(&draw_context, preview_frame) &&
            !hm::preview_overlay::find_overlay_snapshot_meta(preview_frame_meta) &&
            preview_frame_meta->display_meta_list == nullptr,
        "Disabled preview overlays must not allocate a snapshot or production display metadata");
    DsPlayTrackerCtxSetPreviewOverlayFlags(&draw_context, kPreviewOverlayPlay | kPreviewOverlayTransformRequired);
    ok &= expect(
        !draw_context.draw.load(std::memory_order_relaxed),
        "Preview selection must preserve the configured production draw state");
    const bool preview_attached = DsPlayTrackerAttachPreviewSnapshot(&draw_context, preview_frame);
    const auto* preview_snapshot = hm::preview_overlay::find_overlay_snapshot_meta(preview_frame_meta);
    ok &= expect(
        preview_attached && preview_snapshot && !preview_snapshot->play_rects.empty() &&
            preview_frame_meta->display_meta_list == nullptr,
        "Preview play geometry must be snapshotted without contaminating production display metadata");
  }
  if (preview_batch)
    nvds_destroy_batch_meta(preview_batch);
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
  const auto size_tuning = DsPlayTrackerLoadRuntimeTuningContents(R"(
play-tracker:
  hstream-apply-to-fast-box: false
  hstream-apply-to-follower-box: false
  hstream-runtime-tuning:
    ignore-largest-bbox-count: 2
    ignore-oversized-bboxes: true
    oversized-bbox-percent: 100
)");
  auto* original_tracker = draw_tracker.play_tracker.get();
  ok &= expect(
      size_tuning.ok() && DsPlayTrackerCtxApplyRuntimeTuning(&draw_context, *size_tuning).ok() &&
          draw_tracker.play_tracker.get() == original_tracker &&
          draw_tracker.play_tracker_config.ignore_largest_bbox_count == 2 &&
          draw_tracker.play_tracker_config.ignore_oversized_bboxes,
      "Live size filters apply independently of box selection and retain tracker history");
  DsPlayTrackerRuntimeTuning percent_only;
  percent_only.apply_to_follower_box = false;
  percent_only.oversized_bbox_percent = 125;
  ok &= expect(
      DsPlayTrackerCtxApplyRuntimeTuning(&draw_context, percent_only).ok() &&
          draw_tracker.play_tracker_config.ignore_largest_bbox_count == 2 && draw_context.detector_runtime_tuning &&
          draw_context.detector_runtime_tuning->ignore_largest_bbox_count == 2 &&
          draw_context.detector_runtime_tuning->ignore_oversized_bboxes == true &&
          draw_context.detector_runtime_tuning->oversized_bbox_percent == 125,
      "Sparse updates preserve previous size settings for newly created sources and seeks");
  for (const std::string field :
       {"ignore-largest-bbox-count: -1",
        "ignore-largest-bbox-count: 1.5",
        "oversized-bbox-percent: -1",
        "oversized-bbox-percent: .nan",
        "ignore-oversized-bboxes: invalid"}) {
    ok &= expect(
        !DsPlayTrackerLoadRuntimeTuningContents(
             "play-tracker:\n  hstream-apply-to-follower-box: false\n  hstream-runtime-tuning:\n    " + field + "\n")
             .ok(),
        "Invalid live size filtering settings must be rejected");
  }
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
