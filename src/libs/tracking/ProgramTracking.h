#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <opencv2/core.hpp>
#include "absl/status/statusor.h"
#include "hockeymon/csrc/play_tracker/PlayTracker.h"
#include "hstream/src/libs/tracking/NativeTracker.h"
#include "yaml-cpp/yaml.h"

namespace hm::tracking {
struct ProgramObservation {
  std::vector<detection::Proposal> detections;
  std::vector<Observation> tracks;
  std::optional<hm::BBox> crop;
};

// Metadata-only Program processing in authoritative stitched coordinates. The
// caller verifies calibration identity before supplying the saved mask. Frame
// geometry must match exactly; archive/video scaling belongs to the caller's
// explicit coordinate transform. This object never maps or owns video pixels.
class ProgramTracking {
 public:
  static absl::StatusOr<std::unique_ptr<ProgramTracking>> Create(
      const cv::Mat& mask,
      const YAML::Node& camera,
      const TrackerConfig& tracker,
      float raise_center_ratio,
      float lower_bottom_ratio);
  ~ProgramTracking();
  absl::StatusOr<ProgramObservation> Process(
      uint64_t sequence,
      int width,
      int height,
      const std::vector<detection::Proposal>& detections);
  // Clears observation and camera history after a discontinuity; track IDs are
  // never reused. A failed reset leaves the object retired.
  absl::Status Reset();

 private:
  struct Impl;
  explicit ProgramTracking(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
} // namespace hm::tracking
