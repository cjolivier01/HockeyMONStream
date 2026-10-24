#pragma once

#include "hstream/src/libs/gpu/Image.h"
#include "hstream/src/libs/gpu/Runtime.h"
#include "hstream/src/libs/program/FrameGeometry.h"
#include "hstream/src/libs/scoreboard/ScoreboardPlan.h"

namespace hm::playcropper {
struct PackedOutput {
  void* data;
  size_t pitch;
  int width, height;
  gpu::PixelFormat format;
};

// Matrix weights and legal-range scaling in native 8/10-bit code units.
// Resolved from output colorimetry; never selected implicitly by a kernel.
struct YuvEncoding {
  float kr, kb, y_offset, y_range, uv_offset, uv_range;
};
struct YuvOutput {
  void* y;
  void* uv;
  size_t y_pitch, uv_pitch;
  int width, height;
  gpu::PixelFormat format;
  YuvEncoding color;
};

// Fused crop, inverse rotation, bilinear resize, shadow lift and exposure.
// Preserves NVIDIA's x/width sampling and transparent-black out-of-bounds
// behavior, with no tone adjustment to alpha. Every output pixel is written;
// row padding is untouched. RGBA8/RGB8/gray output; RGB/YUV GPU input.
// Input and output must not overlap and remain alive until stream completion,
// including on submission errors. No allocation, sync or video readback.
cudaError_t RenderProgram(
    const gpu::Image& input,
    const PackedOutput& output,
    const FrameTransformGeometry& transform,
    float shadow_lift_percent,
    bool lift_shadow_black_point,
    float exposure,
    cudaStream_t stream);

// Direct NV12/P010 output for hardware encode or GPU preview. Full-precision
// RGB sampling/tone evaluation precedes YUV quantization; chroma is the mean
// over each 2x2 output block (centered chroma). Even output dimensions are
// mandatory. This changes matrix/range only; the caller must preserve the
// source primaries and transfer function, or convert them before rendering.
// An optional scoreboard samples the original panorama after Program tone/crop,
// without a video readback or an intermediate image. Its plan must match both
// source and output dimensions. Source video alpha does not hide the scoreboard.
cudaError_t RenderProgramYuv(
    const gpu::Image& input,
    const YuvOutput& output,
    const FrameTransformGeometry& transform,
    float shadow_lift_percent,
    bool lift_shadow_black_point,
    float exposure,
    cudaStream_t stream,
    const scoreboard::Plan* scoreboard = nullptr);
} // namespace hm::playcropper
