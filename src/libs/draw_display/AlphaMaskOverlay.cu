#include "hstream/src/libs/draw_display/AlphaMaskOverlay.h"

#include <cmath>
#include <limits>

namespace hm::draw_display {
namespace {

__device__ float Sample(AlphaMaskView mask, float u, float v) {
  // Reject far-outside coordinates before converting to integer texel indices.
  // The one-texel border remains available for bilinear edge contributions.
  if (!(u > -1.0F / mask.width && u < 1.0F + 1.0F / mask.width && v > -1.0F / mask.height &&
        v < 1.0F + 1.0F / mask.height))
    return 0;
  const float x = u * mask.width - .5F, y = v * mask.height - .5F;
  const int ix = static_cast<int>(floorf(x)), iy = static_cast<int>(floorf(y));
  const float fx = x - ix, fy = y - iy;
  float coverage = 0;
  for (int j = 0; j < 2; ++j)
    for (int i = 0; i < 2; ++i) {
      const int sx = ix + i, sy = iy + j;
      if (sx >= 0 && sx < mask.width && sy >= 0 && sy < mask.height)
        coverage += mask.data[sy * mask.pitch + sx] * (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
    }
  return coverage / 255.0F;
}

__device__ uint32_t Blend(uint32_t before, float color, float alpha) {
  return static_cast<uint32_t>(floorf(before * (1 - alpha) + color * 255 * alpha + .5F));
}

__global__ void Draw(
    analytics::ImageView output,
    AlphaMaskView mask,
    AlphaMaskMapping mapping,
    analytics::Color color) {
  const unsigned x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= output.width || y >= output.height)
    return;
  const float u = mapping.origin_u + (x + .5F) * mapping.step_x_u + (y + .5F) * mapping.step_y_u;
  const float v = mapping.origin_v + (x + .5F) * mapping.step_x_v + (y + .5F) * mapping.step_y_v;
  const float alpha = color.alpha * Sample(mask, u, v);
  if (!(alpha > 0))
    return;
  auto* pixel = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(output.data) + y * output.pitch) + x;
  const uint32_t before = *pixel;
  *pixel = Blend(before & 255U, color.red, alpha) | (Blend((before >> 8) & 255U, color.green, alpha) << 8) |
      (Blend((before >> 16) & 255U, color.blue, alpha) << 16) | (255U << 24);
}

} // namespace

cudaError_t DrawAlphaMask(
    const analytics::ImageView& output,
    AlphaMaskView mask,
    AlphaMaskMapping mapping,
    analytics::Color color,
    cudaStream_t stream) {
  if (output.format != analytics::PixelFormat::kRgba8 || !output.data || !output.width || !output.height ||
      output.width > 16384 || output.height > 16384 || output.pitch < size_t(output.width) * 4 || output.pitch % 4 ||
      reinterpret_cast<uintptr_t>(output.data) % 4 ||
      output.pitch > std::numeric_limits<size_t>::max() / output.height || !mask.data || !mask.width || !mask.height ||
      mask.width > 12288 || mask.height > 12288 || mask.pitch < mask.width ||
      mask.pitch > (32U * 1024U * 1024U) / mask.height)
    return cudaErrorInvalidValue;
  const float coordinates[] = {
      mapping.origin_u, mapping.origin_v, mapping.step_x_u, mapping.step_x_v, mapping.step_y_u, mapping.step_y_v};
  for (const float coordinate : coordinates)
    if (!std::isfinite(coordinate) || std::abs(coordinate) > 1000000)
      return cudaErrorInvalidValue;
  const float channels[] = {color.red, color.green, color.blue, color.alpha};
  for (const float channel : channels)
    if (!std::isfinite(channel) || channel < 0 || channel > 1)
      return cudaErrorInvalidValue;
  if (color.alpha == 0)
    return cudaSuccess;
  Draw<<<dim3((output.width + 15) / 16, (output.height + 15) / 16), dim3(16, 16), 0, stream>>>(
      output, mask, mapping, color);
  return cudaGetLastError();
}

} // namespace hm::draw_display
