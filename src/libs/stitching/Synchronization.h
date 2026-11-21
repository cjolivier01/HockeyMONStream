#pragma once

#include <string>
#include <vector>

#include "absl/status/statusor.h"

namespace hm::stitching {
struct CameraTrackFormat {
  int width, height, fps_n, fps_d;
  bool constant_rate;
  int64_t timebase_tick_ns;
};
absl::StatusOr<CameraTrackFormat> ReadCameraTrackFormat(const std::string& file);

struct CameraChapterSlice {
  std::string file;
  double start_seconds, duration_seconds;
  // Physical video PTS origin. Slice bounds above are relative to this origin.
  double timestamp_origin_seconds{0};
  // Matroska stream time is relative to the container start, while MP4
  // edit lists express it on the movie timeline.
  double container_clock_origin_seconds{0};
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
