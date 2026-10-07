#include "ScoreboardCuda.h"

namespace hm::scoreboard {
namespace {

__global__ void MakeOpaque(uchar4* pixels, size_t pitch, int width, int height) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height)
    return;
  auto* row = reinterpret_cast<uchar4*>(reinterpret_cast<unsigned char*>(pixels) + y * pitch);
  row[x].w = 255;
}

__global__ void Unpremultiply(uchar4* pixels, size_t pitch, int width, int height) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height)
    return;
  auto* row = reinterpret_cast<uchar4*>(reinterpret_cast<unsigned char*>(pixels) + y * pitch);
  uchar4& pixel = row[x];
  const unsigned alpha = pixel.w;
  if (alpha == 0 || alpha == 255)
    return;
  pixel.x = static_cast<unsigned char>(min(255U, (static_cast<unsigned>(pixel.x) * 255U + alpha / 2U) / alpha));
  pixel.y = static_cast<unsigned char>(min(255U, (static_cast<unsigned>(pixel.y) * 255U + alpha / 2U) / alpha));
  pixel.z = static_cast<unsigned char>(min(255U, (static_cast<unsigned>(pixel.z) * 255U + alpha / 2U) / alpha));
}

bool valid_image(uchar4* pixels, size_t pitch, int width, int height) {
  return pixels && width > 0 && height > 0 && pitch >= static_cast<size_t>(width) * sizeof(uchar4);
}

} // namespace

cudaError_t make_scoreboard_opaque(uchar4* pixels, size_t pitch, int width, int height, cudaStream_t stream) {
  if (!valid_image(pixels, pitch, width, height))
    return cudaErrorInvalidValue;
  const dim3 block(16, 16);
  const dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
  MakeOpaque<<<grid, block, 0, stream>>>(pixels, pitch, width, height);
  return cudaGetLastError();
}

cudaError_t unpremultiply_scoreboard(uchar4* pixels, size_t pitch, int width, int height, cudaStream_t stream) {
  if (!valid_image(pixels, pitch, width, height))
    return cudaErrorInvalidValue;
  const dim3 block(16, 16);
  const dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
  Unpremultiply<<<grid, block, 0, stream>>>(pixels, pitch, width, height);
  return cudaGetLastError();
}

} // namespace hm::scoreboard
