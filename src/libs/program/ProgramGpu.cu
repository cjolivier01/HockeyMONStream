#include "hstream/src/libs/program/ProgramGpu.h"

#include <cmath>
#include "hstream/src/libs/gpu/ImageSampling.cuh"
#include "hstream/src/libs/program/ShadowToneCurve.h"

namespace hm::playcropper {
namespace {
// A bilinear tap is a tent two source pixels wide however far the crop
// reduces, so past some ratio it stops covering the output pixel's footprint
// and the result aliases. samples_x/samples_y spread that many taps across
// the output pixel instead, averaging the footprint it really spans.
//
// Averaging is not free quality. The tent is as wide as a box of sqrt(2)
// source pixels, so a mild reduction is already well covered by one tap and
// extra taps only widen the filter further. N taps spread over (N-1)/N of
// the pixel give an equivalent box of sqrt(2 + R^2 (N^2-1)/N^2) against an
// ideal R. Matching equivalent widths is a sharpness criterion, and on it
// the two filters' error crosses at 16*sqrt(2)/13 = 1.741, so one tap is the
// closer answer below that. Judged on RMS error against the ideal footprint
// instead, averaging would win at every ratio; this picks sharpness, so a
// 1.13x crop keeps one tap.
constexpr float kMinimumAveragedScale = 1.75f;
constexpr int kMaximumAxisSamples = 4;
// Keep host-only BBox operations outside device compilation.
struct Transform {
  float source_x, source_y, anchor_x, anchor_y, left, top, width, height, angle;
  int samples_x, samples_y;
};
// One bilinear tap, with no grading. The single bounds test feeds both the
// fetch and the caller's in-bounds flag, so the two cannot drift apart and
// start counting a tap that contributed transparent black.
__device__ float4 Fetch(
    const gpu::Image& input,
    int x,
    int y,
    float offset_x,
    float offset_y,
    int width,
    int height,
    Transform t,
    bool* inside) {
  const float bx = t.left + (static_cast<float>(x) + offset_x) / width * t.width;
  const float by = t.top + (static_cast<float>(y) + offset_y) / height * t.height;
  const float radians = -t.angle * (M_PI / 180.0f);
  const float sine = sinf(radians), cosine = cosf(radians);
  const float dx = bx - t.anchor_x, dy = by - t.anchor_y;
  const float sx = t.source_x + (t.anchor_x + (dx * cosine - dy * sine));
  const float sy = t.source_y + (t.anchor_y + (dx * sine + dy * cosine));
  const bool in = sx >= 0 && sy >= 0 && sx < input.width - 1 && sy < input.height - 1;
  *inside = in;
  if (!in)
    return make_float4(0, 0, 0, 0);
  const int x0 = static_cast<int>(floorf(sx)), y0 = static_cast<int>(floorf(sy));
  const auto a = gpu::Pixel(input, x0, y0), b = gpu::Pixel(input, x0 + 1, y0);
  const auto c = gpu::Pixel(input, x0, y0 + 1), d = gpu::Pixel(input, x0 + 1, y0 + 1);
  const float fx = sx - x0, fy = sy - y0;
  return make_float4(
      gpu::Mix(a.x, b.x, c.x, d.x, fx, fy),
      gpu::Mix(a.y, b.y, c.y, d.y, fx, fy),
      gpu::Mix(a.z, b.z, c.z, d.z, fx, fy),
      gpu::Mix(a.w, b.w, c.w, d.w, fx, fy));
}
// Grading runs once on the resampled pixel, never per tap. The lift curve is
// concave, so averaging graded taps is not the same as grading the average:
// across a 0-255 edge at full lift the two differ by 17 codes, which would
// make the same content grade differently with the camera's zoom. Applying
// it after the average also keeps it at one evaluation per output pixel
// instead of up to sixteen.
__device__ void Grade(float* result, int channels, float gamma, float amount, bool lift_black, float gain) {
  if (amount > 0 && channels >= 3) {
    float r = result[0] / 255.f, g = result[1] / 255.f, b = result[2] / 255.f;
    evaluate_shadow_lift_rgb(&r, &g, &b, gamma, amount, lift_black);
    result[0] = r * 255.f;
    result[1] = g * 255.f;
    result[2] = b * 255.f;
  } else if (amount > 0) {
    result[0] = evaluate_shadow_lift_luma(result[0] / 255.f, gamma, amount, lift_black) * 255.f;
  }
  if (gain > 1)
    for (int c = 0; c < channels && c < 3; ++c)
      result[c] *= gain;
}
// Rotation sends each sub-step in a different direction but does not change
// its length, so an evenly spaced grid in output space stays evenly spaced
// across the source parallelogram.
//
// Only taps that land in the source are averaged. Fetch returns transparent
// black outside it, and folding that into the mean would darken any pixel
// whose footprint merely reaches the edge. Row 0 does so whenever the crop
// box's top is 0 against a source_rect that starts at the image top, which
// FrameGeometry produces for every no_crop frame and for every frame before
// the first detection; the left column joins it when the camera pans to the
// canvas edge.
// Dividing by the taps actually taken keeps those pixels at full brightness
// and still returns transparent black once no tap lands inside at all.
__device__ float4 SampleArea(
    const gpu::Image& input,
    int x,
    int y,
    int width,
    int height,
    Transform t,
    int channels,
    float gamma,
    float amount,
    bool lift_black,
    float gain) {
  float total[4] = {};
  int taken = 0;
  for (int j = 0; j < t.samples_y; ++j) {
    const float offset_y = (j + .5f) / t.samples_y - .5f;
    for (int i = 0; i < t.samples_x; ++i) {
      const float offset_x = (i + .5f) / t.samples_x - .5f;
      bool inside = false;
      const auto p = Fetch(input, x, y, offset_x, offset_y, width, height, t, &inside);
      if (!inside)
        continue;
      ++taken;
      total[0] += p.x;
      total[1] += p.y;
      total[2] += p.z;
      total[3] += p.w;
    }
  }
  // Defensive rather than observable: dividing by zero would give NaN, and
  // both clamps happen to map that to the same black. Do not rely on it.
  if (!taken)
    return make_float4(0, 0, 0, 0);
  const float inverse = 1.f / taken;
  for (int c = 0; c < 4; ++c)
    total[c] *= inverse;
  Grade(total, channels, gamma, amount, lift_black, gain);
  return make_float4(total[0], total[1], total[2], total[3]);
}
__global__ void Render(
    gpu::Image input,
    PackedOutput output,
    Transform t,
    int channels,
    float gamma,
    float amount,
    bool lift_black,
    float gain) {
  const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
  const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= static_cast<unsigned>(output.width) || y >= static_cast<unsigned>(output.height))
    return;
  const auto p = SampleArea(input, x, y, output.width, output.height, t, channels, gamma, amount, lift_black, gain);
  float result[] = {p.x, p.y, p.z, p.w};
  auto* dest = static_cast<uint8_t*>(output.data) + y * output.pitch + static_cast<size_t>(x) * channels;
  for (int c = 0; c < channels; ++c) {
    // Zero remains zero after rounding, including out-of-image black.
    if (c != 3 && (amount > 0 || gain > 1))
      result[c] += .5f;
    dest[c] = static_cast<uint8_t>(clamp_shadow_value(result[c], 0, 255));
  }
}
__device__ void StoreCode(void* row, int x, float value, bool ten_bit) {
  const auto code = static_cast<unsigned>(clamp_shadow_value(value + .5f, 0, ten_bit ? 1023 : 255));
  if (ten_bit)
    static_cast<uint16_t*>(row)[x] = code << 6;
  else
    static_cast<uint8_t*>(row)[x] = code;
}
__device__ float4 ScoreboardPixel(const gpu::Image& input, int x, int y, const scoreboard::Plan& plan, float4 base) {
  if (!plan.width || x < plan.left || y < plan.top || x - plan.left >= plan.width || y - plan.top >= plan.height)
    return base;
  x -= plan.left;
  y -= plan.top;
  const auto* h = plan.inverse;
  const float denominator = h[6] * x + h[7] * y + h[8];
  const float sx = (h[0] * x + h[1] * y + h[2]) / denominator;
  const float sy = (h[3] * x + h[4] * y + h[5]) / denominator;
  if (!isfinite(sx) || !isfinite(sy) || sx < -.01f || sy < -.01f || sx > input.width - 1 + .01f ||
      sy > input.height - 1 + .01f)
    return base;
  // Only numerical roundoff at a validated image edge is clamped. No source
  // padding or transparent decoder alpha participates in scoreboard sampling.
  const float fx = gpu::Clamp(sx, 0, input.width - 1), fy = gpu::Clamp(sy, 0, input.height - 1);
  const int x0 = static_cast<int>(floorf(fx)), y0 = static_cast<int>(floorf(fy));
  const int x1 = x0 + 1 < input.width ? x0 + 1 : x0;
  const int y1 = y0 + 1 < input.height ? y0 + 1 : y0;
  const auto a = gpu::Pixel(input, x0, y0), b = gpu::Pixel(input, x1, y0);
  const auto c = gpu::Pixel(input, x0, y1), d = gpu::Pixel(input, x1, y1);
  return make_float4(
      gpu::Mix(a.x, b.x, c.x, d.x, fx - x0, fy - y0),
      gpu::Mix(a.y, b.y, c.y, d.y, fx - x0, fy - y0),
      gpu::Mix(a.z, b.z, c.z, d.z, fx - x0, fy - y0),
      255);
}
__global__ void RenderYuv(
    gpu::Image input,
    YuvOutput output,
    Transform t,
    float gamma,
    float amount,
    bool lift_black,
    float gain,
    scoreboard::Plan overlay) {
  const int x = (blockIdx.x * blockDim.x + threadIdx.x) * 2;
  const int y = (blockIdx.y * blockDim.y + threadIdx.y) * 2;
  if (x >= output.width || y >= output.height)
    return;
  const bool ten_bit = output.format == gpu::PixelFormat::kP010;
  const auto color = output.color;
  float u = 0, v = 0;
  for (int j = 0; j < 2; ++j) {
    auto* row = static_cast<uint8_t*>(output.y) + (y + j) * output.y_pitch;
    for (int i = 0; i < 2; ++i) {
      const auto base =
          SampleArea(input, x + i, y + j, output.width, output.height, t, 4, gamma, amount, lift_black, gain);
      const auto p = ScoreboardPixel(input, x + i, y + j, overlay, base);
      const float r = gpu::Clamp(p.x, 0, 255) / 255;
      const float g = gpu::Clamp(p.y, 0, 255) / 255;
      const float b = gpu::Clamp(p.z, 0, 255) / 255;
      const float luma = color.kr * r + (1 - color.kr - color.kb) * g + color.kb * b;
      StoreCode(row, x + i, color.y_offset + color.y_range * luma, ten_bit);
      u += (b - luma) / (2 * (1 - color.kb));
      v += (r - luma) / (2 * (1 - color.kr));
    }
  }
  auto* chroma = static_cast<uint8_t*>(output.uv) + (y / 2) * output.uv_pitch;
  StoreCode(chroma, x, color.uv_offset + color.uv_range * u * .25f, ten_bit);
  StoreCode(chroma, x + 1, color.uv_offset + color.uv_range * v * .25f, ten_bit);
}
// Source pixels per output pixel along one axis, rounded up to whole taps
// once the reduction is steep enough to be worth averaging at all.
int AxisSamples(float source_extent, int output_extent) {
  if (!std::isfinite(source_extent) || output_extent <= 0)
    return 1;
  const float scale = source_extent / output_extent;
  if (!(scale >= kMinimumAveragedScale))
    return 1;
  if (scale >= kMaximumAxisSamples)
    return kMaximumAxisSamples;
  return static_cast<int>(std::ceil(scale));
}
bool Finite(const BBox& box) {
  return std::isfinite(box.left) && std::isfinite(box.top) && std::isfinite(box.right) && std::isfinite(box.bottom) &&
      std::isfinite(box.width()) && std::isfinite(box.height()) && box.width() > 0 && box.height() > 0;
}
bool Overlaps(const void* source, size_t source_bytes, const PackedOutput& output) {
  const auto a = reinterpret_cast<uintptr_t>(source), b = reinterpret_cast<uintptr_t>(output.data);
  return a < b + output.pitch * output.height && b < a + source_bytes;
}
} // namespace
cudaError_t RenderProgram(
    const gpu::Image& input,
    const PackedOutput& output,
    const FrameTransformGeometry& t,
    float lift,
    bool lift_black,
    float exposure,
    cudaStream_t stream) {
  const gpu::Image destination{
      output.data, nullptr, output.pitch, 0, output.width, output.height, output.format, 0, 0, 0, 0, 0, 0, 0};
  const int channels = output.format == gpu::PixelFormat::kRgba8 ? 4
      : output.format == gpu::PixelFormat::kRgb8                 ? 3
      : output.format == gpu::PixelFormat::kGray8                ? 1
                                                                 : 0;
  if (!channels || !gpu::ValidateImage(input) || !gpu::ValidateImage(destination) ||
      (channels == 1 && input.format != gpu::PixelFormat::kGray8) || !Finite(t.source_rect) || !Finite(t.crop_box) ||
      !std::isfinite(t.angle) || !std::isfinite(t.anchor_point.x) || !std::isfinite(t.anchor_point.y) ||
      !std::isfinite(lift) || !std::isfinite(exposure) || Overlaps(input.plane0, input.pitch0 * input.height, output) ||
      ((input.format == gpu::PixelFormat::kNv12 || input.format == gpu::PixelFormat::kP010) &&
       Overlaps(input.plane1, input.pitch1 * (input.height / 2), output)))
    return cudaErrorInvalidValue;
  Transform transform{
      t.source_rect.left,
      t.source_rect.top,
      t.anchor_point.x,
      t.anchor_point.y,
      t.crop_box.left,
      t.crop_box.top,
      t.crop_box.width(),
      t.crop_box.height(),
      t.angle,
      AxisSamples(t.crop_box.width(), output.width),
      AxisSamples(t.crop_box.height(), output.height)};
  const dim3 block(16, 16), grid((output.width + 15) / 16, (output.height + 15) / 16);
  Render<<<grid, block, 0, stream>>>(
      input,
      output,
      transform,
      channels,
      shadow_lift_gamma(lift),
      shadow_lift_amount(lift),
      lift_black,
      exposure_gain(exposure));
  return cudaGetLastError();
}

cudaError_t RenderProgramYuv(
    const gpu::Image& input,
    const YuvOutput& output,
    const FrameTransformGeometry& t,
    float lift,
    bool lift_black,
    float exposure,
    cudaStream_t stream,
    const scoreboard::Plan* overlay) {
  if (overlay &&
      (!scoreboard::ValidatePlan(*overlay) || overlay->source_width != input.width ||
       overlay->source_height != input.height || overlay->output_width != output.width ||
       overlay->output_height != output.height))
    return cudaErrorInvalidValue;
  const gpu::Image destination{
      output.y,
      output.uv,
      output.y_pitch,
      output.uv_pitch,
      output.width,
      output.height,
      output.format,
      0,
      0,
      0,
      0,
      0,
      0,
      0};
  const auto& c = output.color;
  const float maximum = output.format == gpu::PixelFormat::kP010 ? 1023 : 255;
  if ((output.format != gpu::PixelFormat::kNv12 && output.format != gpu::PixelFormat::kP010) ||
      !gpu::ValidateImage(input) || !gpu::ValidateImage(destination) || !Finite(t.source_rect) || !Finite(t.crop_box) ||
      !std::isfinite(t.angle) || !std::isfinite(t.anchor_point.x) || !std::isfinite(t.anchor_point.y) ||
      !std::isfinite(lift) || !std::isfinite(exposure) || !std::isfinite(c.kr) || !std::isfinite(c.kb) || c.kr <= 0 ||
      c.kb <= 0 || c.kr + c.kb >= 1 || !std::isfinite(c.y_offset) || !std::isfinite(c.y_range) ||
      !std::isfinite(c.uv_offset) || !std::isfinite(c.uv_range) || c.y_offset < 0 || c.y_range <= 0 ||
      c.y_offset + c.y_range > maximum || c.uv_range <= 0 || c.uv_offset - c.uv_range / 2 < -.5f ||
      c.uv_offset + c.uv_range / 2 > maximum + .5f)
    return cudaErrorInvalidValue;
  const PackedOutput planes[] = {
      {output.y, output.y_pitch, output.width, output.height, output.format},
      {output.uv, output.uv_pitch, output.width, output.height / 2, output.format}};
  if (Overlaps(output.y, output.y_pitch * output.height, planes[1]))
    return cudaErrorInvalidValue;
  for (const auto& plane : planes) {
    if (Overlaps(input.plane0, input.pitch0 * input.height, plane) ||
        ((input.format == gpu::PixelFormat::kNv12 || input.format == gpu::PixelFormat::kP010) &&
         Overlaps(input.plane1, input.pitch1 * (input.height / 2), plane)))
      return cudaErrorInvalidValue;
  }
  Transform transform{
      t.source_rect.left,
      t.source_rect.top,
      t.anchor_point.x,
      t.anchor_point.y,
      t.crop_box.left,
      t.crop_box.top,
      t.crop_box.width(),
      t.crop_box.height(),
      t.angle,
      AxisSamples(t.crop_box.width(), output.width),
      AxisSamples(t.crop_box.height(), output.height)};
  const dim3 block(16, 16), grid((output.width / 2 + 15) / 16, (output.height / 2 + 15) / 16);
  RenderYuv<<<grid, block, 0, stream>>>(
      input,
      output,
      transform,
      shadow_lift_gamma(lift),
      shadow_lift_amount(lift),
      lift_black,
      exposure_gain(exposure),
      overlay ? *overlay : scoreboard::Plan{});
  return cudaGetLastError();
}
} // namespace hm::playcropper
