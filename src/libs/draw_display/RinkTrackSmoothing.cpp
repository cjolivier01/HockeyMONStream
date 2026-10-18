#include "hstream/src/libs/draw_display/RinkTrackSmoothing.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hm::draw_display {
void RinkTrackSmoothing::Reset() {
  histories_ = {};
}

cv::Point2d RinkTrackSmoothing::Update(uint64_t track, uint64_t pts_ns, uint32_t color, cv::Point2d measured) {
  if (track == std::numeric_limits<uint64_t>::max() || pts_ns == std::numeric_limits<uint64_t>::max() ||
      !std::isfinite(measured.x) || !std::isfinite(measured.y))
    return measured;
  uint64_t key = track;
  key ^= key >> 33;
  key *= 0xff51afd7ed558ccdULL;
  key ^= key >> 33;
  auto& bucket = histories_[key % histories_.size()];
  auto* state = &bucket[0];
  for (auto& candidate : bucket) {
    if (candidate.samples && candidate.track == track) {
      state = &candidate;
      break;
    }
    if (!candidate.samples || candidate.pts_ns < state->pts_ns)
      state = &candidate;
  }
  constexpr uint64_t kMaximumGap = 300000000; // Reacquisition starts at the current measurement.
  if (!state->samples || state->track != track || state->color != color || pts_ns < state->pts_ns ||
      pts_ns - state->pts_ns > kMaximumGap) {
    *state = {track, pts_ns, color, 1, measured, measured, measured, {0, 0}};
    return measured;
  }
  if (pts_ns == state->pts_ns)
    return state->filtered; // Repainting a paused frame must not advance the filter.
  const double dt = double(pts_ns - state->pts_ns) * 1e-9;
  cv::Point2d target = measured;
  if (state->samples >= 2) {
    const auto median = [](double a, double b, double c) {
      return std::max(std::min(a, b), std::min(std::max(a, b), c));
    };
    target = {
        median(measured.x, state->previous.x, state->older.x), median(measured.y, state->previous.y, state->older.y)};
  }
  // A three-sample median removes isolated box-foot spikes. Adaptive low-pass
  // filtering damps stationary jitter and responds faster during real skating.
  const auto derivative = (target - state->filtered) * (1 / dt);
  const double derivative_alpha = dt / (0.15 + dt);
  state->velocity += (derivative - state->velocity) * derivative_alpha;
  const double cutoff = 1.2 + 0.3 * std::min(15.0, cv::norm(state->velocity));
  const double alpha = dt / (1 / (2 * 3.14159265358979323846 * cutoff) + dt);
  state->filtered += (target - state->filtered) * alpha;
  state->older = state->previous;
  state->previous = measured;
  state->pts_ns = pts_ns;
  state->samples = 2;
  return state->filtered;
}
} // namespace hm::draw_display
