#include "hstream/src/libs/stitching/CalibrationReferenceFrames.h"

#include <opencv2/imgcodecs.hpp>
#include <unistd.h>
#include <cmath>
#include <iostream>
#include "hstream/src/libs/common/Process.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/PlayerFrameSelection.h"
#include "yaml-cpp/yaml.h"

int main() {
  using namespace hm::stitching;
  bool ok = true;
  const auto expect = [&](bool value, const char* message) {
    if (!value) {
      std::cerr << message << '\n';
      ok = false;
    }
  };
  const std::array<CameraRecordingWindow, 2> cameras{
      {{{{"left-a", 5, 2}, {"left-b", 0, 3}}, 25, 5}, {{{"right-a", 8, 1}, {"right-b", 0, 4}}, 25, 5}}};
  auto plan = PlanCalibrationReferenceFrames(cameras, -1000000000LL, 5);
  expect(plan.ok(), "Plan wrapped calibration samples");
  if (!plan.ok())
    return 1;
  expect(
      plan->at(0)[0].video == "left-b" && plan->at(0)[0].seconds == 2 && plan->at(0)[1].video == "right-b" &&
          plan->at(0)[1].seconds == 3,
      "Reference must be one second before common end");
  expect(
      plan->at(1)[0].video == "left-a" && plan->at(1)[0].seconds == 5 && plan->at(1)[1].video == "right-a" &&
          plan->at(1)[1].seconds == 8,
      "Additional pairs must wrap to synchronized starts");
  expect(!PlanCalibrationReferenceFrames(cameras, -6000000000LL, 1).ok(), "Reject reference before recording start");
  const auto ffmpeg = hm::findExecutable("ffmpeg", {"PATH"});
  if (!ffmpeg)
    return 1;
  const auto game = std::filesystem::temp_directory_path() / ("calibration-reference-" + std::to_string(getpid()));
  std::filesystem::create_directories(game);
  const auto video = game / "camera.mp4";
  const int generated = hm::run_command(
      {*ffmpeg,
       "-v",
       "error",
       "-f",
       "lavfi",
       "-i",
       "testsrc2=size=64x48:rate=25:duration=4",
       "-c:v",
       "libx264",
       "-g",
       "25",
       "-pix_fmt",
       "yuv420p",
       video.string()},
      "",
      {},
      {});
  if (generated != 0)
    return 1;
  auto config = YAML::Load(
      "game: {videos: {left: [camera.mp4], right: [camera.mp4]}, stitching: {frame_offsets: {left: 0, right: 25}}}\nstitching: {stitch_frame_time: '-00:00:01'}");
  auto inputs = EndRelativeCalibrationInputs(game, config, 3, {});
  expect(inputs.ok(), "Prepare lazy extraction with independent effective offsets");
  if (!inputs.ok()) {
    std::cerr << inputs.status() << '\n';
    return 1;
  }
  const double expected[3][2]{{2, 3}, {0, 1}, {1, 2}};
  for (size_t i = 0; i < inputs->size(); ++i) {
    for (size_t camera = 0; camera < 2; ++camera) {
      const auto image = game / (std::to_string(i) + "-" + std::to_string(camera) + ".png");
      const auto status = (camera == 0 ? inputs->at(i).left : inputs->at(i).right)(image);
      expect(status.ok(), "Decode selected still with a keyframe seek");
      if (!status.ok())
        std::cerr << status << '\n';
      expect(
          std::abs(inputs->at(i).source(camera).seconds - expected[i][camera]) < 0.001,
          "Saved source provenance must be the actual decoded physical timestamp");
      expect(!cv::imread(image.string(), cv::IMREAD_UNCHANGED).empty(), "Extracted PNG must be readable");
    }
  }
  config["stitching"]["stitch_frame_time"] = "-00:00:00.001";
  auto last = EndRelativeCalibrationInputs(game, config, 1, {});
  expect(
      last.ok() && last->front().left(game / "last.png").ok() &&
          std::abs(last->front().source(0).seconds - 2.96) < 0.001,
      "A reference inside the final frame duration must retain that last frame");
  auto cancelled = EndRelativeCalibrationInputs(game, config, 1, [] { return true; });
  const auto output = game / "cancelled.png";
  expect(
      cancelled.ok() && absl::IsCancelled(cancelled->front().left(output)) && !std::filesystem::exists(output),
      "Cancellation must prevent extraction and publication");
  config["stitching"]["stitch_frame_time"] = "-00:00:01";
  const auto right_video = game / "right-camera.mp4";
  std::filesystem::copy_file(video, right_video);
  config["game"]["videos"]["right"][0] = "right-camera.mp4";
  PlayerFrameSelectionSettings selected_settings;
  selected_settings.frame_count = 2;
  PlayerFrameSelectionContext context{
      {"source_context", *player_frame_source_context(config, 2000000000ULL)},
      {"baseline_generation", "baseline"},
      {"output_generation", "output"},
      {"detector_identity", "detector"},
      {"rink_mask_sha256", "mask"},
      {"rink_mask_revision", "revision"},
      {"fieldmask_settings", "settings"},
      {"output_rotation_degrees", "0"},
      {"decode_anchor_ns", "2000000000"},
      {"scan_clock", "wrapped-elapsed-v1"},
      {"scan_reference_ns", "-1000000000"},
      {"scan_recording_duration_ns", "3000000000"},
      {"scan_playback_start_ns", "520000000"}};
  std::vector<PlayerFrameObservation> observations(2);
  for (size_t index = 0; index < observations.size(); ++index) {
    auto& sample = observations[index];
    const uint64_t pts = index ? 520000000ULL : 2000000000ULL;
    sample.pair.cameras = {{{video.string(), pts, 0, index}, {right_video.string(), pts + 1000000000ULL, 1, index}}};
    sample.pair.timeline_pts_ns = index ? 1000000000ULL : 0;
    sample.eligible_people = 1;
    sample.quality = 1;
    sample.coverage = {static_cast<uint16_t>(index)};
    sample.size_band_counts = {1, 0, 0};
  }
  auto selected = SelectPlayerFrames(
      selected_settings, observations, {*BindPlayerFrameSource(video), *BindPlayerFrameSource(right_video)}, context);
  expect(selected.ok() && selected->plan.has_value(), "Select physical pairs on both sides of the wrap");
  if (!selected.ok() || !selected->plan)
    return 1;
  config["stitching"]["calibration_frame_selection"] = PlayerFrameSelectionPlanYaml(*selected->plan);
  auto exact = EndRelativeCalibrationInputs(game, config, 2, {});
  expect(
      exact.ok() && exact->at(0).left(game / "selected-tail.png").ok() &&
          exact->at(1).left(game / "selected-wrap.png").ok() && std::abs(exact->at(1).source(0).seconds - .52) < 1e-9,
      "Wrapped selection must extract its exact chosen PTS, not ordinary circular samples");
  selected->plan->selected.front().pair.cameras[0].source_pts_ns += 1000000;
  selected->plan->fingerprint = *PlayerFrameSelectionFingerprint(*selected->plan);
  config["stitching"]["calibration_frame_selection"] = PlayerFrameSelectionPlanYaml(*selected->plan);
  auto wrong = EndRelativeCalibrationInputs(game, config, 2, {});
  expect(
      wrong.ok() && !wrong->front().left(game / "wrong-selected.png").ok(),
      "An unavailable exact selected PTS must not silently choose the next frame");
  config["stitching"].remove("calibration_frame_selection");
  const auto shifted = game / "shifted.mp4";
  expect(
      hm::run_command(
          {*ffmpeg,
           "-v",
           "error",
           "-f",
           "lavfi",
           "-i",
           "testsrc2=size=64x48:rate=25:duration=4",
           "-f",
           "lavfi",
           "-i",
           "anullsrc=r=48000:cl=mono:d=6",
           "-vf",
           "setpts=PTS+2/TB",
           "-fps_mode",
           "passthrough",
           "-c:v",
           "libx264",
           "-g",
           "25",
           "-c:a",
           "aac",
           shifted.string()},
          "",
          {},
          {}) == 0,
      "Generate video whose PTS origin differs from the audio/container origin");
  config["game"]["videos"]["left"][0] = "shifted.mp4";
  config["game"]["videos"]["right"][0] = "shifted.mp4";
  config["game"]["stitching"]["frame_offsets"]["right"] = 0;
  config["stitching"]["stitch_frame_time"] = "-00:00:01";
  auto shifted_inputs = EndRelativeCalibrationInputs(game, config, 1, {});
  expect(shifted_inputs.ok(), "Plan an end-relative frame on the video clock");
  if (shifted_inputs.ok()) {
    const auto status = shifted_inputs->front().left(game / "shifted.png");
    if (!status.ok())
      std::cerr << status << '\n';
    expect(
        status.ok() && std::abs(shifted_inputs->front().source(0).seconds - 5) < 0.001,
        "A one-second end-relative reference must select physical PTS5 from video starting at PTS2");
  }
  std::filesystem::remove_all(game);
  return ok ? 0 : 1;
}
