#include "ScoreboardSharpen.h"

#include <cuda_runtime.h>

namespace hm::scoreboard {
namespace {

__device__ uchar4 read_pixel(const uchar4* source, size_t pitch, int x, int y) {
  return reinterpret_cast<const uchar4*>(reinterpret_cast<const unsigned char*>(source) + y * pitch)[x];
}

__device__ unsigned char sharpen_channel(unsigned char center, float blurred, float amount) {
  return static_cast<unsigned char>(fminf(255.0F, fmaxf(0.0F, center + amount * (center - blurred))) + 0.5F);
}

__global__ void sharpen_kernel(
    const uchar4* source,
    size_t source_pitch,
    uchar4* destination,
    size_t destination_pitch,
    int width,
    int height,
    float amount) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height)
    return;

  float red = 0.0F, green = 0.0F, blue = 0.0F;
  for (int dy = -1; dy <= 1; ++dy) {
    const int sy = max(0, min(height - 1, y + dy));
    const float wy = dy == 0 ? 2.0F : 1.0F;
    for (int dx = -1; dx <= 1; ++dx) {
      const int sx = max(0, min(width - 1, x + dx));
      const float weight = wy * (dx == 0 ? 2.0F : 1.0F);
      const uchar4 pixel = read_pixel(source, source_pitch, sx, sy);
      red += weight * pixel.x;
      green += weight * pixel.y;
      blue += weight * pixel.z;
    }
  }
  const uchar4 center = read_pixel(source, source_pitch, x, y);
  uchar4 result;
  result.x = sharpen_channel(center.x, red / 16.0F, amount);
  result.y = sharpen_channel(center.y, green / 16.0F, amount);
  result.z = sharpen_channel(center.z, blue / 16.0F, amount);
  result.w = center.w;
  reinterpret_cast<uchar4*>(reinterpret_cast<unsigned char*>(destination) + y * destination_pitch)[x] = result;
}

} // namespace

cudaError_t sharpen_scoreboard(
    const uchar4* source,
    size_t source_pitch,
    uchar4* destination,
    size_t destination_pitch,
    int width,
    int height,
    float amount,
    cudaStream_t stream) {
  if (!source || !destination || source == destination || width <= 0 || height <= 0 || amount < 0.0F || amount > 100.0F ||
      source_pitch < static_cast<size_t>(width) * sizeof(uchar4) ||
      destination_pitch < static_cast<size_t>(width) * sizeof(uchar4))
    return cudaErrorInvalidValue;
  constexpr dim3 block(16, 16);
  const dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
  sharpen_kernel<<<grid, block, 0, stream>>>(
      source, source_pitch, destination, destination_pitch, width, height, amount);
  return cudaGetLastError();
}

} // namespace hm::scoreboard
