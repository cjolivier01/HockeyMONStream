#include "hstream/src/libs/tracking/CameraSeekState.h"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace hm::play_tracker;

void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

PlayTrackerConfig Config() {
  PlayTrackerConfig config;
  config.ignore_largest_bbox = false;
  config.play_detector.max_positions = 24;
  config.play_detector.max_velocity_positions = 8;
  for (const char* name : {"fast", "follower"}) {
    AllLivingBoxConfig box;
    box.name = name;
    box.arena_box = hm::BBox(0, 0, 2000, 1000);
    box.min_width = 300;
    box.min_height = 180;
    box.max_width = 2000;
    box.max_height = 1000;
    box.max_speed_x = 8;
    box.max_speed_y = 3;
    box.max_accel_x = 0.75f;
    box.max_accel_y = 0.5f;
    box.max_speed_w = 10;
    box.max_speed_h = 8;
    box.max_accel_w = 0.4f;
    box.max_accel_h = 0.4f;
    config.living_boxes.push_back(box);
  }
  return config;
}

PlayTrackerResults Step(PlayTracker& tracker, size_t frame, size_t epoch) {
  std::vector<size_t> ids;
  std::vector<hm::BBox> boxes;
  for (size_t i = 0; i < 8; ++i) {
    ids.push_back(epoch + i);
    const float x = 200 + frame * 5 + i * 40;
    boxes.emplace_back(x, 350, x + 25, 440);
  }
  return tracker.forward(ids, boxes);
}

bool Equal(const hm::BBox& a, const hm::BBox& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
} // namespace

int main() {
  try {
    PlayTracker live(hm::BBox(0, 0, 2000, 1000), Config());
    for (size_t frame = 0; frame < 90; ++frame)
      Step(live, frame, 10);
    live.get_live_box(1)->set_translation_constraints(31, 4, 1.3f, 0.7f);
    const auto captured = hm::tracking::CaptureCameraSeekState(live);
    Check(captured.ok(), "capture the moved, live-tuned camera");
    Check(deserialize_snapshot(*captured).detector.tracks.empty(), "retire old object identities in the handoff");

    auto qt_state = live.snapshot();
    qt_state.detector.tracks.clear();
    auto qt = PlayTracker::from_snapshot(qt_state);
    PlayTracker cold(hm::BBox(0, 0, 2000, 1000), Config());
    auto prepared = hm::tracking::PrepareCameraSeekState(*captured, cold.snapshot(), true);
    Check(prepared.ok(), "restore the full-rebuild camera before its first frame");
    auto browser = PlayTracker::from_snapshot(*prepared);
    bool cold_differs = false;
    for (size_t frame = 0; frame < 12; ++frame) {
      const auto expected = Step(*qt, frame, 1000);
      const auto actual = Step(*browser, frame, 1000);
      const auto old_rebuild = Step(cold, frame, 1000);
      Check(expected.tracking_boxes.size() == actual.tracking_boxes.size(), "replacement camera box count");
      for (size_t i = 0; i < expected.tracking_boxes.size(); ++i) {
        Check(Equal(expected.tracking_boxes[i], actual.tracking_boxes[i]), "first or continued post-seek camera drift");
        cold_differs |= !Equal(expected.tracking_boxes[i], old_rebuild.tracking_boxes[i]);
      }
    }
    Check(cold_differs, "regression sequence must distinguish the old cold-start reconstruction");

    // A recorded seed borrows position/dynamics, while today's live tuning wins.
    auto current = live.snapshot();
    current.living_boxes[1].config.max_speed_x = 42;
    auto recording = hm::tracking::PrepareCameraSeekState(*captured, current);
    Check(
        recording.ok() && recording->living_boxes[1].config.max_speed_x == 42,
        "recording must preserve current tuning");
    const auto live_before_rejections = serialize_snapshot(live.snapshot());
    auto wrong_role = current;
    wrong_role.living_boxes[1].config.name = "another-role";
    Check(!hm::tracking::PrepareCameraSeekState(*captured, wrong_role).ok(), "reject a different camera role");
    auto wrong_arena = current;
    wrong_arena.living_boxes[1].config.arena_box->right += 1;
    Check(!hm::tracking::PrepareCameraSeekState(*captured, wrong_arena).ok(), "reject different camera geometry");
    Check(!hm::tracking::PrepareCameraSeekState("broken", current).ok(), "reject malformed state");
    Check(
        !hm::tracking::PrepareCameraSeekState(std::string(hm::tracking::kMaximumCameraSeekStateBytes + 1, 'x'), current)
             .ok(),
        "bound checkpoint parsing");
    Check(
        serialize_snapshot(live.snapshot()) == live_before_rejections,
        "failed validation must not change the live camera");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
