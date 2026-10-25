#pragma once

#include "hstream/src/libs/draw_display/AnalyticsOverlay.h"

namespace hm::draw_display {

// Immutable, device-resident single-channel coverage. The caller retains this
// allocation and the independently owned writable RGBA output until its stream
// completes. The texture budget is shared with bounded preview mask decoding.
struct AlphaMaskView {
  const uint8_t* data;
  size_t pitch;
  uint32_t width, height;
};

// Affine normalized texture coordinates at output pixel edges:
// uv = origin + (x + .5) * step_x + (y + .5) * step_y.
// This also represents the inverse rotated/cropped Program mapping. Sampling
// matches GL_LINEAR with a transparent GL_CLAMP_TO_BORDER, including half texels.
struct AlphaMaskMapping {
  float origin_u, origin_v;
  float step_x_u, step_x_v;
  float step_y_u, step_y_v;
};

// No uploads, synchronization, allocation, or readback. Work is bounded by the
// destination subimage, so the caller can exclude letterbox bars by construction.
// CUDA and HIP use the same kernel. RGBA video is treated as opaque, like the
// native preview's video pass; RGB uses straight source-over and alpha stays255.
cudaError_t DrawAlphaMask(
    const analytics::ImageView& output,
    AlphaMaskView mask,
    AlphaMaskMapping mapping,
    analytics::Color color,
    cudaStream_t stream);

} // namespace hm::draw_display
