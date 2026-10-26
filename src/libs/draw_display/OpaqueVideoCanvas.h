#pragma once

#include "hstream/src/libs/draw_display/AnalyticsOverlay.h"

namespace hm::draw_display {

struct VideoRectangle {
  unsigned left, top, width, height;
};

// Finish an independently owned RGBA video canvas after GPU fitting: clear
// letterbox bars and make video alpha opaque in one pass. Native display and
// H.264 carry RGB video, so transparent panorama corners must not change the
// source-over color of later drawings. Source/archive allocations are untouched.
// CUDA/HIP share the same kernel; no allocation, synchronization or readback.
cudaError_t FinishOpaqueVideoCanvas(const analytics::ImageView& output, VideoRectangle video, cudaStream_t stream);

} // namespace hm::draw_display
