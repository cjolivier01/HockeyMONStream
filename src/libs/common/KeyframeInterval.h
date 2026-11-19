#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

namespace hm {

// A keyframe interval configured in seconds becomes whole encoder frames. Both
// GPU backends must land on the same count for the same recording, so the
// division happens here rather than in each caller: computing seconds * n / d
// and seconds * (n / d) disagree by a frame at the halfway point, which at
// 60000/1001 fps and 1.226225 seconds is 73.5 rounding to 74 one way and
// 73.499999999999986 rounding to 73 the other.
//
// Returns nothing when the inputs cannot name a usable interval, so callers
// fail closed instead of silently keeping an encoder default.
inline std::optional<uint32_t> keyframe_interval_frames(double seconds, double frames_per_second) {
  if (!(seconds > 0.0) || !std::isfinite(seconds))
    return std::nullopt;
  if (!(frames_per_second > 0.0) || !std::isfinite(frames_per_second))
    return std::nullopt;
  const double frames = std::round(seconds * frames_per_second);
  if (!(frames >= 1.0) || frames > 2147483647.0)
    return std::nullopt;
  return static_cast<uint32_t>(frames);
}

// The exact rational form a container reports. Reduced to one double first so
// it agrees with the overload above.
inline std::optional<uint32_t> keyframe_interval_frames(double seconds, int fps_numerator, int fps_denominator) {
  if (fps_numerator <= 0 || fps_denominator <= 0)
    return std::nullopt;
  return keyframe_interval_frames(seconds, static_cast<double>(fps_numerator) / static_cast<double>(fps_denominator));
}

} // namespace hm
