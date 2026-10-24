#include "hstream/src/libs/media/StereoFramePlan.h"

#include <cmath>
#include <limits>

namespace hm::media {
absl::StatusOr<StereoFramePlan> PlanStereoFrames(
    double start_seconds,
    double left_offset_frames,
    double right_offset_frames,
    int fps_n,
    int fps_d) {
  if (!std::isfinite(start_seconds) || start_seconds < 0 || !std::isfinite(left_offset_frames) ||
      left_offset_frames < 0 || !std::isfinite(right_offset_frames) || right_offset_frames < 0 || fps_n <= 0 ||
      fps_d <= 0)
    return absl::InvalidArgumentError(
        "Stereo frame planning requires nonnegative finite time/offsets and positive fps");
  // Decimal YAML/CLI values arrive as doubles. Keep arithmetic in long
  // double, but remove uncertainty at integer/half-frame boundaries caused
  // by the original double rounding (e.g. .04 seconds at 25 fps, or .6 - .1).
  const long double start_frames = static_cast<long double>(start_seconds) * fps_n / fps_d;
  const long double epsilon = 2 * std::numeric_limits<double>::epsilon();
  const auto snap = [](long double value, long double boundary, long double error) {
    return std::abs(value - boundary) <= error ? boundary : value;
  };
  const long double left_position = start_frames + left_offset_frames;
  const long double left_error = epsilon * (start_frames + left_offset_frames);
  const long double relative = static_cast<long double>(left_offset_frames) - right_offset_frames;
  const long double relative_error = epsilon * (static_cast<long double>(left_offset_frames) + right_offset_frames);
  if (left_error >= .25L || relative_error >= .25L)
    return absl::OutOfRangeError("Stereo inputs have insufficient precision to select a frame");
  const long double left = std::ceil(snap(left_position, std::round(left_position), left_error));
  const long double difference = std::round(snap(relative, std::round(relative * 2) / 2, relative_error));
  const long double right = left - difference;
  if (left > std::numeric_limits<int64_t>::max() || right < 0 || right > std::numeric_limits<int64_t>::max())
    return absl::OutOfRangeError("Stereo frame index exceeds its clock");
  const auto left_index = static_cast<uint64_t>(left), right_index = static_cast<uint64_t>(right);
  const auto time = [=](uint64_t index) { return static_cast<__int128>(index) * fps_d * 1000000000 / fps_n; };
  const auto left_time = time(left_index), right_time = time(right_index);
  if (left_time > std::numeric_limits<int64_t>::max() || right_time > std::numeric_limits<int64_t>::max())
    return absl::OutOfRangeError("Stereo frame time exceeds its clock");
  return StereoFramePlan{left_index, right_index, static_cast<uint64_t>(left_time), static_cast<uint64_t>(right_time)};
}
} // namespace hm::media
