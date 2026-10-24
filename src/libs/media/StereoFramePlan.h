#pragma once

#include <cstdint>

#include "absl/status/statusor.h"

namespace hm::media {
struct StereoFramePlan {
  uint64_t left_frame, right_frame;
  uint64_t left_time_ns, right_time_ns;
};
// Select the first left frame at/after the requested synchronized start, then
// the nearest matching right frame. Half-frame ties round the relative frame
// offset away from zero. Seeks land on the source frame grid: fractional seek
// positions let GStreamer clip the first PTS without changing its pixels.
// Both streams must have this same constant rational frame rate.
absl::StatusOr<StereoFramePlan> PlanStereoFrames(
    double start_seconds,
    double left_offset_frames,
    double right_offset_frames,
    int fps_n,
    int fps_d);
} // namespace hm::media
