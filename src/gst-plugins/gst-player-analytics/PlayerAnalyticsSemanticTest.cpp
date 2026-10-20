// Opt-in native integration using actual prepared engines and hockey pixels.
// Repeating a real pose image tests causal history and execution, not accuracy
// on temporal actions. Pixel upload here is offline fixture setup only.
#include <cuda_runtime_api.h>
#include <gst/gst.h>
#include <nvbufsurface.h>
#include <nvdsmeta.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "hstream/src/gst-plugins/gst-player-analytics/PlayerAnalyticsProcessor.h"
#include "hstream/src/libs/player_analytics/FrameMeta.h"

namespace pa = hm::player_analytics;
namespace {
void Require(bool okay, const char* message) {
  if (!okay) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

struct Fixture {
  NvBufSurface* surface{nullptr};
  unsigned width{0}, height{0};
  explicit Fixture(const cv::Mat& bgr) {
    Require(!bgr.empty(), "Missing fixture pixels");
    cv::Mat rgba;
    cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);
    width = rgba.cols;
    height = rgba.rows;
    NvBufSurfaceCreateParams params{};
    params.gpuId = 0;
    params.width = width;
    params.height = height;
    params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
    params.layout = NVBUF_LAYOUT_PITCH;
    // CUDA device surfaces are available on Jetson too; the separate plugin
    // lifecycle test exercises native EGL surface arrays on that platform.
    params.memType = NVBUF_MEM_CUDA_DEVICE;
    Require(NvBufSurfaceCreate(&surface, 1, &params) == 0, "Cannot allocate fixture surface");
    surface->numFilled = 1;
    const auto& plane = surface->surfaceList[0];
    Require(
        cudaMemcpy2D(plane.dataPtr, plane.pitch, rgba.data, rgba.step, width * 4, height, cudaMemcpyHostToDevice) ==
            cudaSuccess,
        "Cannot upload fixture");
  }
  ~Fixture() {
    if (surface)
      NvBufSurfaceDestroy(surface);
  }
};

struct Detection {
  uint64_t id;
  pa::Box box;
};
constexpr uint64_t kId = (uint64_t{1} << 40) + 7;
const pa::Box kPoseBox{485, 282, 603, 1850};
// Epsilon moves floating-point crop endpoints within the desired floor/ceil
// bins: the actual torso crop is [160,260,410,435] in the fixture surface.
const pa::Box kJerseyBox{23.334F, 104.644F, 683.332F, 621.427F};

pa::FrameResult Process(
    pa::PlayerAnalyticsProcessor& processor,
    Fixture& image,
    uint64_t pts,
    int sequence,
    const std::vector<Detection>& objects,
    uint32_t source_id = 7) {
  auto* batch = nvds_create_batch_meta(1);
  Require(batch != nullptr, "Cannot allocate batch metadata");
  auto* frame = nvds_acquire_frame_meta_from_pool(batch);
  frame->source_id = source_id;
  frame->source_frame_width = image.width;
  frame->source_frame_height = image.height;
  frame->frame_num = sequence;
  frame->buf_pts = pts;
  nvds_add_frame_meta_to_batch(batch, frame);
  for (const auto& detection : objects) {
    auto* object = nvds_acquire_obj_meta_from_pool(batch);
    object->object_id = detection.id;
    object->class_id = 0;
    object->rect_params.left = detection.box.left;
    object->rect_params.top = detection.box.top;
    object->rect_params.width = detection.box.width;
    object->rect_params.height = detection.box.height;
    nvds_add_obj_meta_to_frame(frame, object, nullptr);
  }
  const auto status = processor.Process(image.surface, batch);
  if (!status.ok())
    std::cerr << status << '\n';
  Require(status.ok(), "Native semantic processing failed");
  const auto* metadata = pa::FindFrameResult(frame);
  if (metadata)
    Require(pa::ValidateFrameResult(*metadata), "Invalid published semantic metadata");
  const auto result = metadata ? *metadata : pa::FrameResult{};
  nvds_destroy_batch_meta(batch);
  return result;
}

std::unique_ptr<pa::PlayerAnalyticsProcessor> Create(const pa::Config& config) {
  auto processor = pa::PlayerAnalyticsProcessor::Create(config, 0);
  if (!processor.ok())
    std::cerr << processor.status() << '\n';
  Require(processor.ok() && *processor, "Cannot create native semantic processor");
  return std::move(*processor);
}

pa::Config Base() {
  pa::Config config;
  config.maximum_tracks = 4;
  config.maximum_due_rois = 3;
  config.batch_size = 1;
  return config;
}

void DrawingAndCadence(const char* bundle, Fixture& image, const pa::Box& box) {
  auto config = Base();
  config.pose.enabled = true;
  config.pose.bundle = bundle;
  config.pose.rate_hz = 1; // Deliberately longer than the bounded display hold.
  auto processor = Create(config);
  const auto fresh = Process(*processor, image, pa::kSecond, 0, {{kId, box}});
  Require(fresh.player_count == 1 && fresh.players[0].has_pose, "Missing initial pose observation");
  auto moved = box;
  moved.left += 5;
  moved.width *= 1.1F;
  const auto held = Process(*processor, image, pa::kSecond + 33333333, 1, {{kId, moved}});
  Require(
      held.player_count == 1 && !held.players[0].has_pose && held.players[0].pose_observed_at == pa::kInvalidTime &&
          held.players[0].retained_pose.observed_at == pa::kSecond && processor->counters().pose_results == 1,
      "Display retention created a fresh inference observation");
  for (size_t i = 0; i < fresh.players[0].pose.size(); ++i) {
    const auto& point = fresh.players[0].pose[i];
    const auto& retained = held.players[0].retained_pose.pose[i];
    Require(retained.confidence == point.confidence, "Display retention changed joint confidence");
    if (point.confidence > 0)
      Require(
          std::abs(retained.x - (moved.left + (point.x - box.left) * moved.width / box.width)) < .01F &&
              std::abs(retained.y - point.y) < .01F,
          "Retained skeleton did not follow the current tracked box");
  }
  Require(
      Process(*processor, image, pa::kSecond + pa::kPoseDrawingMaximumAge, 2, {{kId, box}}).player_count == 1 &&
          Process(*processor, image, pa::kSecond + pa::kPoseDrawingMaximumAge + 1, 3, {{kId, box}}).player_count == 0,
      "Display retention expiry boundary failed");
  Process(*processor, image, 2 * pa::kSecond, 4, {{kId, box}});
  Process(*processor, image, 2 * pa::kSecond + 10000000, 5, {});
  Require(
      Process(*processor, image, 2 * pa::kSecond + 20000000, 6, {{kId, box}}).player_count == 0,
      "Reappearing track inherited its missing-track display pose");
  processor->Reset(false);
  Require(
      Process(*processor, image, 2 * pa::kSecond + 30000000, 7, {{kId, box}}).players[0].has_pose,
      "Reset failed to retire cadence/display state");
  const auto seek = Process(*processor, image, pa::kSecond, 8, {{kId, box}});
  Require(
      seek.players[0].has_pose && seek.players[0].retained_pose.observed_at == pa::kInvalidTime,
      "Seek published retained display state");

  config.continuous_pose_drawing = false;
  processor = Create(config);
  Process(*processor, image, 0, 0, {{kId, box}});
  Require(
      Process(*processor, image, 33333333, 1, {{kId, box}}).player_count == 0,
      "Continuous-drawing off retained a skeleton");

  auto yaml = YAML::Load("pose: {enable: true, model: custom, rate-hz: fps}\nbatch-size: 1");
  yaml["pose"]["bundle"] = bundle;
  const auto parsed = pa::ParseConfig(yaml);
  Require(parsed.ok(), "Cannot parse every-frame pose config");
  processor = Create(*parsed);
  // Fractional 30/60 fps intervals, a 1 ns step, and a long gap all admit work.
  const std::array<uint64_t, 6> times{{0, 33333333, 66700000, 83383333, 83383334, pa::kSecond}};
  for (size_t i = 0; i < times.size(); ++i) {
    const auto result = Process(*processor, image, times[i], i, {{kId, box}});
    Require(
        result.player_count == 1 && result.players[0].has_pose && result.players[0].pose_observed_at == times[i] &&
            processor->counters().pose_results == i + 1,
        "fps cadence skipped an incoming frame");
  }
  Process(*processor, image, times.back(), times.size(), {{kId, box}});
  Require(processor->counters().pose_results == times.size(), "Duplicate PTS ran every-frame inference twice");
  std::cout << "Continuous pose follows boxes, expires, clears on track loss/seek/reset; off and fps cadence passed\n";
}

void Jersey(const char* bundle, Fixture& image) {
  auto config = Base();
  config.jersey.enabled = true;
  config.jersey.bundle = bundle;
  config.maximum_due_rois = 1;
  config.maximum_tracks = 1;
  // Disabled features are intentionally invalid and inaccessible.
  config.pose.bundle = config.action.bundle = "/proc/1/inaccessible-model";
  config.pose.rate_hz = config.action.rate_hz = -1;
  auto processor = Create(config);
  const std::vector<Detection> detections{{kId, kJerseyBox}};
  auto first = Process(*processor, image, 0, 0, detections);
  Require(
      first.player_count == 0 && processor->counters().jersey_results == 1,
      "One jersey observation prematurely formed consensus");
  const auto duplicate = Process(*processor, image, 0, 1, detections);
  Require(
      duplicate.player_count == 0 && processor->counters().jersey_results == 1,
      "Duplicate PTS contributed a jersey observation");
  auto voted = Process(*processor, image, 500000000, 2, detections);
  Require(
      voted.player_count == 1 && std::string(voted.players[0].jersey.text.data()) == "27" && !voted.players[0].has_pose,
      "Real jersey 27 consensus missing or leaked pose");
  const auto observed = voted.players[0].jersey.observed_at;
  auto retained = Process(*processor, image, 550000000, 3, detections);
  Require(
      retained.player_count == 1 && retained.players[0].jersey.observed_at == observed &&
          !retained.players[0].has_pose && processor->counters().jersey_results == 2,
      "Skipped inference lost/renewed retained jersey label");
  // A new identity cannot steal a retained admission or its label.
  auto excluded = Process(*processor, image, 600000000, 4, {{kId + 1, kJerseyBox}});
  Require(
      excluded.player_count == 0 && processor->counters().capacity_excluded == 1,
      "Capacity exclusion published another identity's jersey");
  auto reappeared = Process(*processor, image, 650000000, 5, detections);
  Require(
      reappeared.player_count == 1 && reappeared.players[0].jersey.observed_at == observed,
      "Brief jersey absence lost retained identity");
  // Age while the identity is visible but its torso is too small to infer.
  const pa::Box tiny{200, 200, 10, 10};
  for (int i = 1; i <= 4; ++i)
    retained = Process(*processor, image, 650000000 + i * pa::kSecond, 5 + i, {{kId, tiny}});
  Require(
      retained.player_count == 0 && processor->counters().jersey_results == 2 &&
          processor->counters().jersey_visibility_skipped == 4,
      "Jersey expiry/visibility gating failed");
  // A long absence and reuse needs new consensus, even with the same uint64 ID.
  Process(*processor, image, 5 * pa::kSecond, 10, {});
  auto reused = Process(*processor, image, 8 * pa::kSecond, 11, detections);
  Require(reused.player_count == 0, "Expired track reused prior jersey consensus");
  voted = Process(*processor, image, 8500000000, 12, detections);
  Require(voted.player_count == 1, "New incarnation could not build jersey consensus");
  const auto changed_source = Process(*processor, image, 8550000000, 13, detections, 8);
  Require(changed_source.player_count == 0, "Source identity change retained jersey consensus");
  auto invalid = Process(*processor, image, pa::kInvalidTime, 13, detections);
  Require(invalid.player_count == 0 && processor->counters().invalid_time == 1, "Invalid PTS retained jersey");
  auto restarted = Process(*processor, image, 9 * pa::kSecond, 14, detections);
  Require(restarted.player_count == 0, "Invalid PTS failed to clear jersey evidence");
  Process(*processor, image, 9500000000, 15, detections);
  const auto seek = Process(*processor, image, pa::kSecond, 16, detections);
  Require(seek.player_count == 0, "Backward seek retained jersey evidence");
  Require(
      processor->counters().pose_enqueues == 0 && processor->counters().action_enqueues == 0 &&
          processor->counters().maximum_frame_samples == 1,
      "Jersey-only mode executed another model");
  std::cout
      << "Real jersey 27 consensus, retained labels, duplicate/invalid PTS, capacity, expiry and ID reuse passed\n";
}

void Actions(const char* pose, const char* jersey, const char* action, Fixture& image) {
  auto config = Base();
  config.maximum_due_rois = 6;
  config.batch_size = 2;
  config.pose.enabled = config.jersey.enabled = config.action.enabled = true;
  config.pose.bundle = pose;
  config.jersey.bundle = jersey;
  config.action.bundle = action;
  config.action.confidence_threshold = 0;
  config.action.rate_hz = 240; // Even when due, an unchanged window cannot vote twice.
  auto processor = Create(config);
  const std::vector<Detection> detections{{kId, kPoseBox}, {kId + 1, kPoseBox}};
  pa::FrameResult result;
  for (size_t frame = 0; frame <= 100; ++frame) {
    result = Process(*processor, image, frame * pa::kActionSamplePeriod, frame, detections);
    Require(
        result.player_count == 2 && result.players[0].has_pose,
        "Actual hockey fixture did not publish its same-frame pose");
    if (frame < 100)
      Require(
          processor->counters().action_enqueues == 0 && result.players[0].action.label < 0,
          "Action inferred before complete 100-sample causal history");
  }
  Require(
      processor->counters().action_enqueues == 1 && processor->counters().action_results == 2 &&
          result.players[0].action.label >= 0 && result.players[0].action.observed_at == 10 * pa::kSecond &&
          result.players[0].action.observed_at - result.players[0].action.window_start == 9900000000,
      "Complete causal history did not execute the actual action engine");
  Require(
      ((result.players[0].track_id == kId && result.players[1].track_id == kId + 1) ||
       (result.players[0].track_id == kId + 1 && result.players[1].track_id == kId)) &&
          result.players[1].action.label == result.players[0].action.label &&
          result.players[1].action.observed_at == result.players[0].action.observed_at,
      "Batched action output lost its separate full 64-bit track identity");
  const auto label = result.players[0].action;
  const auto samples = processor->counters().model_samples;
  result = Process(*processor, image, 10050000000, 101, detections);
  Require(
      result.player_count == 2 && !result.players[0].has_pose &&
          result.players[0].pose_observed_at == pa::kInvalidTime && result.players[0].action.label == label.label &&
          result.players[0].action.observed_at == label.observed_at && processor->counters().model_samples == samples,
      "Between-inference action label missing, renewed, or accompanied by stale pose geometry");
  auto duplicate = Process(*processor, image, 10050000000, 102, detections);
  Require(
      duplicate.player_count == 0 && processor->counters().model_samples == samples,
      "Duplicate PTS produced semantic evidence");
  result = Process(*processor, image, 10400000000, 103, detections);
  Require(
      result.players[0].action.label < 0 && processor->counters().action_history_resets >= 1 &&
          processor->counters().action_enqueues == 1,
      "Pose gap retained an action label/window");
  result = Process(*processor, image, 0, 104, detections);
  Require(result.players[0].action.label < 0, "Backward seek reused action evidence");
  Process(*processor, image, pa::kInvalidTime, 105, detections);
  result = Process(*processor, image, pa::kActionSamplePeriod, 106, detections);
  Require(result.players[0].action.label < 0, "Invalid timestamp reused action evidence");
  processor->Reset();
  result = Process(*processor, image, 2 * pa::kActionSamplePeriod, 107, detections);
  Require(
      result.players[0].action.label < 0 && processor->counters().maximum_frame_samples == 6 &&
          processor->counters().jersey_enqueues > 0,
      "Reset or aggregate all-model budget failed");
  std::cout << "Two actual poses -> 100 uniform samples/9.9 s each -> batched actual action label=" << label.label
            << " confidence=" << label.confidence << "; gaps, seeks and label-only frames passed\n";
}

void Capacity(const char* pose, const char* action, Fixture& image) {
  auto config = Base();
  config.pose.enabled = config.action.enabled = true;
  config.pose.bundle = pose;
  config.action.bundle = action;
  config.maximum_due_rois = 1;
  config.maximum_tracks = 2;
  auto processor = Create(config);
  std::array<size_t, 2> counts{};
  for (int frame = 0; frame < 105; ++frame) {
    const auto result = Process(
        *processor,
        image,
        frame * pa::kActionSamplePeriod,
        frame,
        {{kId, kPoseBox}, {kId + 1, kPoseBox}, {kId + 2, kPoseBox}});
    Require(result.player_count == (frame ? 2 : 1), "Budget deferral lost continuous pose drawing");
    size_t fresh_count = 0;
    for (size_t i = 0; i < result.player_count; ++i) {
      const auto& player = result.players[i];
      Require(player.action.label < 0, "Display retention created unsupported action evidence");
      Require(
          player.track_id == kId || player.track_id == kId + 1,
          "Capacity-limited scheduler admitted an excluded identity");
      if (player.has_pose) {
        ++fresh_count;
        ++counts[player.track_id - kId];
      } else {
        Require(player.retained_pose.observed_at != pa::kInvalidTime, "Deferred pose has no display estimate");
      }
    }
    Require(fresh_count == 1, "Budget deferral created extra pose evidence");
  }
  const auto& counters = processor->counters();
  Require(
      counts[0] == 53 && counts[1] == 52 && counters.maximum_frame_samples == 1 && counters.action_enqueues == 0 &&
          counters.budget_deferred > 0 && counters.action_history_unready > 0 && counters.action_history_resets > 0 &&
          counters.capacity_excluded == 105,
      "Fair bounded admission/history-gap reporting failed");
  std::cout << "105 real pose frames under one-sample capacity: fair 53/52, action unready without synthesized poses\n";
}

void Guided(const char* pose, const char* jersey, Fixture& real_image) {
  auto config = Base();
  config.pose.enabled = config.jersey.enabled = true;
  config.pose.bundle = pose;
  config.jersey.bundle = jersey;
  config.jersey_roi_mode = pa::JerseyRoiMode::kPose;
  config.jersey.rate_hz = 20;
  config.maximum_due_rois = 2;
  Fixture blank(cv::Mat::zeros(240, 320, CV_8UC3));
  auto processor = Create(config);
  const auto result = Process(*processor, blank, 0, 0, {{kId, {60, 20, 100, 200}}});
  Require(
      result.player_count == 1 && result.players[0].has_pose && processor->counters().pose_enqueues == 1 &&
          processor->counters().jersey_enqueues == 0 && processor->counters().jersey_pose_skipped == 1,
      "Insufficient same-frame torso joints fell back to bbox OCR");
  Process(*processor, real_image, 500000000, 1, {{kId, kPoseBox}});
  Require(
      processor->counters().jersey_enqueues == 1 && processor->counters().maximum_frame_samples == 2,
      "Valid actual same-frame torso pose did not run guided jersey within its pair budget");
  Process(*processor, real_image, 550000000, 2, {{kId, kPoseBox}});
  Require(
      processor->counters().jersey_enqueues == 1 && processor->counters().pose_enqueues == 2,
      "Jersey faster than pose cadence reused old joints for a fresh crop");
  Process(*processor, real_image, 600000000, 3, {{kId, kPoseBox}});
  Require(
      processor->counters().jersey_enqueues == 2 && processor->counters().pose_enqueues == 3,
      "Guided pair did not resume on fresh pose cadence");
  std::cout << "Pose-guided jersey requires fresh joints: valid pairs, no stale joints or bbox fallback passed\n";
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (argc == 4 && std::string(argv[3]) == "--pose-drawing") {
    Fixture image(cv::imread(argv[2]));
    DrawingAndCadence(argv[1], image, {0, 0, float(image.width), float(image.height)});
    return 0;
  }
  if (argc != 6) {
    std::cerr << "Usage: PlayerAnalyticsSemanticTest POSE_BUNDLE JERSEY_BUNDLE ACTION_BUNDLE POSE_SCENE JERSEY_SCENE\n";
    std::cerr << "   or: PlayerAnalyticsSemanticTest POSE_BUNDLE POSE_SCENE --pose-drawing\n";
    return 1;
  }
  Fixture pose_image(cv::imread(argv[4]));
  Fixture jersey_image(cv::imread(argv[5]));
  DrawingAndCadence(argv[1], pose_image, kPoseBox);
  Jersey(argv[2], jersey_image);
  Actions(argv[1], argv[2], argv[3], pose_image);
  Capacity(argv[1], argv[3], pose_image);
  Guided(argv[1], argv[2], pose_image);
}
