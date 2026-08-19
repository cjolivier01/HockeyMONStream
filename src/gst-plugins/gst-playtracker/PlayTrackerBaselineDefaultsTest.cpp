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
  const std::filesystem::path validation_path = std::filesystem::temp_directory_path() /
      ("playtracker-baseline-validation-" + std::to_string(::getpid()) + ".yaml");
  YAML::Node document(YAML::NodeType::Map);
  document["play-tracker"] = YAML::Clone(yaml);
  std::ofstream(validation_path) << YAML::Dump(document) << '\n';
  const absl::Status valid_status = DsPlayTrackerValidateConfigFile(validation_path.string());
  document["play-tracker"]["no-wide-start"] = "not-a-boolean";
  std::ofstream(validation_path) << YAML::Dump(document) << '\n';
  const absl::Status malformed_status = DsPlayTrackerValidateConfigFile(validation_path.string());
  std::filesystem::remove(validation_path);
  ok &= expect(
      valid_status.ok() && !malformed_status.ok(),
      "Native validation must require and type-check every baseline-backed tracker field");
  return ok ? 0 : 1;
}
