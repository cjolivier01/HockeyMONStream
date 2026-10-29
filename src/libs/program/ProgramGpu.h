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
using YuvEncoding = gpu::YuvEncoding;
struct YuvOutput {
  void* y;
  void* uv;
  size_t y_pitch, uv_pitch;
  int width, height;
  gpu::PixelFormat format;
  YuvEncoding color;
};

// Fused crop, inverse rotation, resize, shadow lift and exposure. The tap
// count is chosen per axis, not per crop: an axis reducing by 1.75x or more
// averages ceil(ratio) taps, capped at 4, and every other axis keeps the
// single bilinear tap. So a crop may magnify in x and average 3 taps in y.
// The 1.75 floor is deliberate. A bilinear tap is already as wide as a box
// of sqrt(2) source pixels, and on equivalent width the two filters' error
// crosses at 1.741, so below there one tap is the closer answer and
// averaging would only soften it; the floor rounds that up. Averaging covers
// the footprint the output pixel spans instead of leaving most of it to a
// tap too narrow to reach. Past 4x the cap stops widening and the shortfall
// returns; past 8x the tents stop overlapping at all.
// Taps are centered on the existing x/width corner convention, so an
// averaged axis reaches 0.5 - 0.5/N of a source step outside crop_box at the
// low edge and stops 0.5 + 0.5/N short at the high edge - 0.375 and 0.625 at
// four taps. The displacement from the old single tap is the same at both.
// Preserves NVIDIA's x/width sampling. Transparent-black out-of-bounds
// behavior holds only once no tap lands inside: a pixel whose footprint
// merely reaches the edge keeps the brightness and alpha of the taps that
// did land, so the opaque region grows by up to 0.375 of an output pixel
// across an axis-aligned source edge, and up to sqrt(2) times that at a
// corner or a rotated one.
// Shadow lift and exposure run once on the resampled pixel, never per tap:
// the lift curve is concave, so grading taps and averaging them is not the
// same as grading the average.
// No tone adjustment to alpha. Every output pixel is written;
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
