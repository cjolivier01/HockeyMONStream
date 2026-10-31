#pragma once

#include <string>
#include <vector>

#include "absl/status/statusor.h"

namespace hm::stitching {
struct CameraChapterSlice {
  std::string file;
  double start_seconds, duration_seconds;
};
struct CameraRecordingWindow {
  std::vector<CameraChapterSlice> chapters;
  double fps, duration_seconds;
};
// Probe chapter metadata and locate a bounded window on the concatenated video
// clock. No video frames before the requested source start are decoded.
absl::StatusOr<CameraRecordingWindow> CameraRecordingWindowAt(
    const std::vector<std::string>& files,
    double start_seconds,
    double duration_seconds);

// Returns the residual skip in seconds after each independently positioned
// window. The caller converts it using each camera's own frame rate.
absl::StatusOr<std::pair<double, double>> synchronize_by_audio(
    const CameraRecordingWindow& left,
    const CameraRecordingWindow& right);
} // namespace hm::stitching
