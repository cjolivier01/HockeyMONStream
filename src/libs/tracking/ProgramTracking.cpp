#include "hstream/src/libs/tracking/ProgramTracking.h"

#include <algorithm>
#include <cmath>

#include "hstream/src/libs/detection/RinkDetectionMask.h"
#include "hstream/src/libs/tracking/ProgramCameraConfig.h"

namespace hm::tracking {
struct ProgramRinkFilter::Impl {
  cv::Mat mask;
  cv::Size size;
  size_t maximum_detections;
  cv::Point2f centroid;
  hm::BBox arena;
  float raise, lower;
};
ProgramRinkFilter::ProgramRinkFilter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ProgramRinkFilter::~ProgramRinkFilter() = default;
absl::StatusOr<std::unique_ptr<ProgramRinkFilter>> ProgramRinkFilter::Create(
    const cv::Mat& mask,
    size_t maximum_detections,
    float raise,
    float lower) {
  if (mask.empty() || mask.type() != CV_8UC1 || !maximum_detections || !std::isfinite(raise) || !std::isfinite(lower))
    return absl::InvalidArgumentError("Program requires a calibration mask and finite mask bounds");
  try {
    auto impl = std::make_unique<Impl>();
    impl->size = mask.size();
    impl->maximum_detections = maximum_detections;
    cv::Rect field;
    impl->centroid = detection::compute_centroid(mask, field);
    impl->mask = detection::convert_to_bit_mask(mask);
    impl->arena = gst_hm_playtracker::program_camera_arena(
        hm::BBox(0, 0, mask.cols, mask.rows),
        hm::BBox(field.x, field.y, field.x + field.width, field.y + field.height));
    impl->raise = raise;
    impl->lower = lower;
    return std::unique_ptr<ProgramRinkFilter>(new ProgramRinkFilter(std::move(impl)));
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Program mask preparation: ") + error.what());
  }
}
hm::BBox ProgramRinkFilter::arena() const {
  return impl_->arena;
}
absl::StatusOr<std::vector<detection::Proposal>> ProgramRinkFilter::Process(
    int width,
    int height,
    const std::vector<detection::Proposal>& detections) const {
  const auto& impl = *impl_;
  if (width != impl.size.width || height != impl.size.height)
    return absl::InvalidArgumentError("Program frame geometry differs from its calibration mask");
  if (detections.size() > impl.maximum_detections)
    return absl::ResourceExhaustedError("Detection count exceeds Program tracking bound");
  std::vector<detection::Proposal> output;
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
      output.push_back(p);
  }
  return output;
}

struct ProgramCamera::Impl {
  hm::BBox arena;
  hm::play_tracker::PlayTrackerConfig config;
  std::unique_ptr<hm::play_tracker::PlayTracker> camera;
  bool observed{false}, retired{false};
  bool ignore_referees{false}, ignore_goalies{false};
  int referee{-1}, goalie{-1};
};
ProgramCamera::ProgramCamera(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ProgramCamera::~ProgramCamera() = default;
absl::StatusOr<std::unique_ptr<ProgramCamera>> ProgramCamera::Create(const hm::BBox& arena, const YAML::Node& config) {
  try {
    std::string error;
    if (!gst_hm_playtracker::validate_numeric_yaml_fields(config, &error))
      return absl::InvalidArgumentError(error);
    auto impl = std::make_unique<Impl>();
    impl->arena = arena;
    impl->ignore_referees = config["ignore-referees"].as<bool>();
    impl->ignore_goalies = config["ignore-goalies"].as<bool>();
    if (const auto roles = config["person-roles"]) {
      impl->referee = roles["referee"].as<int>();
      impl->goalie = roles["goalie"].as<int>();
    }
    impl->config = gst_hm_playtracker::create_play_tracker_config(arena, config);
    if (impl->config.living_boxes.empty())
      return absl::InvalidArgumentError("Program requires a configured camera follower");
    impl->camera = std::make_unique<hm::play_tracker::PlayTracker>(arena, impl->config);
    return std::unique_ptr<ProgramCamera>(new ProgramCamera(std::move(impl)));
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Program camera preparation: ") + error.what());
  }
}
absl::Status ProgramCamera::Reset() {
  auto& impl = *impl_;
  impl.retired = true;
  try {
    impl.camera = std::make_unique<hm::play_tracker::PlayTracker>(impl.arena, impl.config);
    impl.observed = false;
    impl.retired = false;
    return absl::OkStatus();
  } catch (const std::exception& error) {
    return absl::InternalError(std::string("Program camera reset: ") + error.what());
  }
}
absl::StatusOr<std::optional<hm::BBox>> ProgramCamera::Process(const std::vector<Observation>& tracks) {
  auto& impl = *impl_;
  if (impl.retired)
    return absl::FailedPreconditionError("Reset Program camera after processing failure");
  impl.retired = true;
  try {
    std::vector<size_t> ids;
    std::vector<hm::BBox> boxes;
    for (const auto& track : tracks) {
      const auto& p = track.box;
      // Only players steer the camera, matching DsPlayTrackerProcessFrame.
      if (p.class_id != 0 && (p.class_id != impl.referee || impl.ignore_referees) &&
          (p.class_id != impl.goalie || impl.ignore_goalies))
        continue;
      ids.push_back(track.id);
      boxes.emplace_back(p.left, p.top, p.right, p.bottom);
    }
    impl.observed |= !boxes.empty();
    std::optional<hm::BBox> crop;
    if (impl.observed) {
      auto result = impl.camera->forward(ids, boxes);
      if (result.tracking_boxes.empty())
        return absl::InternalError("Program camera produced no follower box");
      crop = result.tracking_boxes.back();
    }
    impl.retired = false;
    return crop;
  } catch (const std::exception& error) {
    return absl::InternalError(std::string("Program camera: ") + error.what());
  }
}

struct ProgramTracking::Impl {
  std::unique_ptr<ProgramRinkFilter> mask;
  std::unique_ptr<NativeTracker> tracker;
  std::unique_ptr<ProgramCamera> camera;
  bool retired{false};
};
ProgramTracking::ProgramTracking(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ProgramTracking::~ProgramTracking() = default;
absl::StatusOr<std::unique_ptr<ProgramTracking>> ProgramTracking::Create(
    const cv::Mat& mask,
    const YAML::Node& camera,
    const TrackerConfig& tracker,
    float raise,
    float lower) {
  auto filter = ProgramRinkFilter::Create(mask, tracker.maximum_tracks, raise, lower);
  if (!filter.ok())
    return filter.status();
  auto association = NativeTracker::Create(tracker);
  if (!association.ok())
    return association.status();
  auto follower = ProgramCamera::Create((*filter)->arena(), camera);
  if (!follower.ok())
    return follower.status();
  auto impl = std::make_unique<Impl>();
  impl->mask = std::move(*filter);
  impl->tracker = std::move(*association);
  impl->camera = std::move(*follower);
  return std::unique_ptr<ProgramTracking>(new ProgramTracking(std::move(impl)));
}
absl::Status ProgramTracking::Reset() {
  auto& impl = *impl_;
  impl.retired = true;
  const auto status = impl.camera->Reset();
  if (!status.ok())
    return status;
  impl.tracker->Reset();
  impl.retired = false;
  return absl::OkStatus();
}
absl::StatusOr<ProgramObservation> ProgramTracking::Process(
    uint64_t sequence,
    int width,
    int height,
    const std::vector<detection::Proposal>& detections) {
  auto& impl = *impl_;
  if (impl.retired)
    return absl::FailedPreconditionError("Reset Program tracking after processing failure");
  auto filtered = impl.mask->Process(width, height, detections);
  if (!filtered.ok())
    return filtered.status();
  impl.retired = true;
  auto tracks = impl.tracker->Update(sequence, *filtered);
  if (!tracks.ok())
    return tracks.status();
  auto crop = impl.camera->Process(*tracks);
  if (!crop.ok())
    return crop.status();
  impl.retired = false;
  return ProgramObservation{std::move(*filtered), std::move(*tracks), *crop};
}
} // namespace hm::tracking
