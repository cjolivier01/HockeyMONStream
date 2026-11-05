#include "hstream/src/libs/tracking/ProgramTracking.h"
#include "hstream/src/libs/tracking/ProgramCameraConfig.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
} // namespace
int main() {
  try {
    const YAML::Node yaml = YAML::Load(R"(
camera-name: GoPro
no-wide-start: false
ignore-referees: false
ignore-goalies: false
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

    for (int role : {1, 2}) {
      for (bool excluded : {false, true}) {
        auto role_config = YAML::Clone(yaml);
        role_config["person-roles"]["goalie"] = 1;
        role_config["person-roles"]["referee"] = 2;
        role_config[role == 1 ? "ignore-goalies" : "ignore-referees"] = excluded;
        auto camera = hm::tracking::ProgramCamera::Create(hm::BBox(0, 0, 2000, 1000), role_config);
        Check(camera.ok(), camera.status().ToString());
        auto output = (*camera)->Process({{1, {400, 200, 440, 300, .9f, role, 0}}});
        Check(
            output.ok() && output->has_value() != excluded,
            "Role camera inclusion did not follow its independent switch");
      }
    }
    cv::Mat mask(1000, 2000, CV_8UC1, cv::Scalar(0));
    mask(cv::Rect(100, 200, 1800, 600)).setTo(255);
    const hm::tracking::TrackerConfig tuning{.25, .6, .7, .8, .5, .7, true, 30, 2, 512, 8 * 1024 * 1024};
    auto created = hm::tracking::ProgramTracking::Create(mask, yaml, tuning, -.1f, .1f);
    Check(created.ok(), created.status().ToString());
    auto program = std::move(*created);
    Check(!program->Process(0, 2001, 1000, {}).ok(), "reject incompatible mask geometry");
    auto empty = program->Process(0, 2000, 1000, {});
    Check(empty.ok() && !empty->crop, "empty initial frames do not seed camera");
    const std::vector<hm::detection::Proposal> officials{{1600, 400, 1640, 500, .9f, 1, 4}};
    auto official_first = program->Process(1, 2000, 1000, officials);
    Check(official_first.ok() && !official_first->crop, "tentative officials do not seed camera");
    auto official_second = program->Process(2, 2000, 1000, officials);
    Check(
        official_second.ok() && official_second->tracks.size() == 1 && !official_second->crop,
        "confirmed officials retain tracks without seeding camera");
    Check(program->Reset().ok(), "reset official-only sequence");
    Check(program->Process(0, 2000, 1000, {}).ok(), "start mixed-class sequence");
    std::vector<hm::detection::Proposal> detections{
        {400, 200, 440, 300, .9f, 0, 0},
        {600, 250, 640, 350, .9f, 0, 1},
        {800, 300, 840, 400, .9f, 0, 2},
        {20, 10, 60, 80, .9f, 0, 3},
        officials.front()};
    auto first = program->Process(1, 2000, 1000, detections);
    Check(
        first.ok() && first->detections.size() == 4 && first->tracks.empty() && !first->crop,
        "mask spectators before tracking and wait for confirmed tracks");
    auto second = program->Process(2, 2000, 1000, detections);
    Check(second.ok() && second->tracks.size() == 4 && second->crop, "confirmed players drive Program camera");
    const auto first_id = second->tracks.front().id;
    const auto arena =
        gst_hm_playtracker::program_camera_arena(hm::BBox(0, 0, 2000, 1000), hm::BBox(100, 200, 1899, 799));
    hm::play_tracker::PlayTracker reference(arena, gst_hm_playtracker::create_play_tracker_config(arena, yaml));
    // Compare each composed step against the existing native camera engine,
    // using the actual track identities produced by the new association stage.
    for (uint64_t frame = 2; frame < 15; ++frame) {
      auto output = frame == 2 ? std::move(second) : program->Process(frame, 2000, 1000, detections);
      Check(output.ok() && output->crop, "Program retains camera output");
      std::vector<size_t> ids;
      std::vector<hm::BBox> boxes;
      for (const auto& track : output->tracks) {
        const auto& p = track.box;
        if (p.class_id != 0)
          continue;
        ids.push_back(track.id);
        boxes.emplace_back(p.left, p.top, p.right, p.bottom);
      }
      const auto expected = reference.forward(ids, boxes).tracking_boxes.back();
      const auto actual = *output->crop;
      Check(
          actual.left == expected.left && actual.top == expected.top && actual.right == expected.right &&
              actual.bottom == expected.bottom,
          "native camera parity with mixed-class tracks");
      Check(
          std::isfinite(actual.left) && std::isfinite(actual.top) && actual.width() > 0 && actual.height() > 0,
          "finite positive camera crop");
      for (auto& p : detections) {
        p.left += 2;
        p.right += 2;
      }
    }
    auto lost = program->Process(15, 2000, 1000, {});
    Check(lost.ok() && lost->tracks.empty() && lost->crop, "lost predictions do not become observations");
    Check(!program->Process(17, 2000, 1000, {}).ok(), "discontinuities require reset");
    Check(!program->Process(16, 2000, 1000, {}).ok(), "failed processing retires Program state");
    Check(program->Reset().ok(), "reset Program history");
    empty = program->Process(0, 2000, 1000, {});
    Check(empty.ok() && !empty->crop, "reset clears camera initialization");
    Check(program->Process(1, 2000, 1000, detections).ok(), "new tentative tracks");
    second = program->Process(2, 2000, 1000, detections);
    Check(
        second.ok() && !second->tracks.empty() && second->tracks.front().id > first_id,
        "reset preserves monotonic identities");
    std::cout << "Program mask, tracking, native camera parity and reset checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
