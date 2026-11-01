#include "hstream/src/libs/stitching/CalibrationReferenceFrames.h"

#include <opencv2/imgcodecs.hpp>
#include <unistd.h>
#include <cmath>
#include <iostream>
#include "hstream/src/libs/common/Process.h"
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
