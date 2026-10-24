#include "hstream/src/libs/tracking/ProgramTracking.h"

#include <algorithm>
#include <cmath>

#include "hstream/src/libs/detection/RinkDetectionMask.h"
#include "hstream/src/libs/tracking/ProgramCameraConfig.h"

namespace hm::tracking {
struct ProgramTracking::Impl {
  cv::Mat mask;
  cv::Size size;
  size_t maximum_detections;
  cv::Point2f centroid;
  hm::BBox arena;
  float raise, lower;
  hm::play_tracker::PlayTrackerConfig camera_config;
  std::unique_ptr<hm::play_tracker::PlayTracker> camera;
  std::unique_ptr<NativeTracker> tracker;
  bool observed{false}, retired{false};
};
ProgramTracking::ProgramTracking(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ProgramTracking::~ProgramTracking() = default;

absl::StatusOr<std::unique_ptr<ProgramTracking>> ProgramTracking::Create(
    const cv::Mat& mask,
    const YAML::Node& camera,
    const TrackerConfig& tracker,
    float raise,
    float lower) {
  if (mask.empty() || mask.type() != CV_8UC1 || !std::isfinite(raise) || !std::isfinite(lower))
    return absl::InvalidArgumentError("Program requires a nonempty calibration mask and finite mask adjustments");
  auto created = NativeTracker::Create(tracker);
  if (!created.ok())
    return created.status();
  try {
    std::string error;
    if (!gst_hm_playtracker::validate_numeric_yaml_fields(camera, &error))
      return absl::InvalidArgumentError(error);
    auto impl = std::make_unique<Impl>();
    impl->size = mask.size();
    impl->maximum_detections = tracker.maximum_tracks;
    cv::Rect field;
    impl->centroid = detection::compute_centroid(mask, field);
    impl->mask = detection::convert_to_bit_mask(mask);
    impl->arena = gst_hm_playtracker::program_camera_arena(
        hm::BBox(0, 0, mask.cols, mask.rows),
        hm::BBox(field.x, field.y, field.x + field.width, field.y + field.height));
    impl->raise = raise;
    impl->lower = lower;
    impl->camera_config = gst_hm_playtracker::create_play_tracker_config(impl->arena, camera);
    if (impl->camera_config.living_boxes.empty())
      return absl::InvalidArgumentError("Program requires a configured camera follower");
    impl->camera = std::make_unique<hm::play_tracker::PlayTracker>(impl->arena, impl->camera_config);
    impl->tracker = std::move(*created);
    return std::unique_ptr<ProgramTracking>(new ProgramTracking(std::move(impl)));
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Program tracking preparation: ") + error.what());
  }
}

absl::Status ProgramTracking::Reset() {
  auto& impl = *impl_;
  impl.retired = true;
  try {
    auto camera = std::make_unique<hm::play_tracker::PlayTracker>(impl.arena, impl.camera_config);
    impl.tracker->Reset();
    impl.camera = std::move(camera);
    impl.observed = false;
    impl.retired = false;
    return absl::OkStatus();
  } catch (const std::exception& error) {
    return absl::InternalError(std::string("Program tracking reset: ") + error.what());
  }
}

absl::StatusOr<ProgramObservation> ProgramTracking::Process(
    uint64_t sequence,
    int width,
    int height,
    const std::vector<detection::Proposal>& detections) {
  auto& impl = *impl_;
  if (impl.retired)
    return absl::FailedPreconditionError("Reset Program tracking after processing failure");
  if (width != impl.size.width || height != impl.size.height)
    return absl::InvalidArgumentError("Program frame geometry differs from its calibration mask");
  if (detections.size() > impl.maximum_detections)
    return absl::ResourceExhaustedError("Detection count exceeds Program tracking bound");
  try {
    ProgramObservation output;
    for (const auto& p : detections) {
      const auto decision = detection::EvaluateRinkDetection(
          impl.mask,
          impl.size,
          impl.centroid,
          {p.left, p.top, p.right - p.left, p.bottom - p.top},
          {1, 1},
          impl.raise,
          impl.lower);
      if (decision.keep)
        output.detections.push_back(p);
    }
    auto tracked = impl.tracker->Update(sequence, output.detections);
    if (!tracked.ok()) {
      impl.retired = true;
      return tracked.status();
    }
    output.tracks = std::move(*tracked);
    std::vector<size_t> ids;
    std::vector<hm::BBox> boxes;
    for (const auto& track : output.tracks) {
      const auto& p = track.box;
      // Match DsPlayTrackerProcessFrame: only players steer the camera.
      // Other classes retain their identities for metadata consumers.
      if (p.class_id != 0)
        continue;
      ids.push_back(track.id);
      boxes.emplace_back(p.left, p.top, p.right, p.bottom);
    }
    // Match the existing native policy's delayed initialization: empty frames
    // before the first confirmed track cannot seed a fictitious camera state.
    impl.observed |= !boxes.empty();
    if (impl.observed) {
      auto result = impl.camera->forward(ids, boxes);
      if (result.tracking_boxes.empty()) {
        impl.retired = true;
        return absl::InternalError("Program camera produced no follower box");
      }
      output.crop = result.tracking_boxes.back();
    }
    return output;
  } catch (const std::exception& error) {
    impl.retired = true;
    return absl::InternalError(std::string("Program tracking: ") + error.what());
  }
}
} // namespace hm::tracking
