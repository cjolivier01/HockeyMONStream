#include "hstream/src/libs/detection/DetectorGpu.h"
#include "hstream/src/libs/gpu/ImageSampling.cuh"

#include <cmath>
#include <limits>

namespace hm::detection {
namespace {
using gpu::Clamp;
using gpu::Mix;
using gpu::Pixel;
__global__ void Preprocess(Image image, Letterbox box, float* output) {
  const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
  const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= static_cast<unsigned>(box.width) || y >= static_cast<unsigned>(box.height))
    return;
  float3 rgb = make_float3(0, 0, 0);
  if (x >= box.left && y >= box.top && x < box.left + box.resized_width && y < box.top + box.resized_height) {
    const float sx = Clamp((x - box.left + .5f) * image.width / box.resized_width - .5f, 0, image.width - 1);
    const float sy = Clamp((y - box.top + .5f) * image.height / box.resized_height - .5f, 0, image.height - 1);
    const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
    const int x1 = min(x0 + 1, image.width - 1), y1 = min(y0 + 1, image.height - 1);
    const auto a = Pixel(image, x0, y0), b = Pixel(image, x1, y0), c = Pixel(image, x0, y1), d = Pixel(image, x1, y1);
    const float dx = sx - x0, dy = sy - y0;
    rgb =
        make_float3(Mix(a.x, b.x, c.x, d.x, dx, dy), Mix(a.y, b.y, c.y, d.y, dx, dy), Mix(a.z, b.z, c.z, d.z, dx, dy));
  }
  const size_t size = static_cast<size_t>(box.width) * box.height;
  const size_t index = static_cast<size_t>(y) * box.width + x;
  output[index] = rgb.x * box.scale;
  output[index + size] = rgb.y * box.scale;
  output[index + 2 * size] = rgb.z * box.scale;
}
__global__ void Filter(
    const float* predictions,
    uint32_t rows,
    int classes,
    float threshold,
    int width,
    int height,
    Proposal* proposals,
    uint32_t* count) {
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= rows)
    return;
  const float* p = predictions + static_cast<size_t>(i) * 6;
  for (int j = 0; j < 6; ++j)
    if (!isfinite(p[j]))
      return;
  if (p[4] < threshold || p[4] > 1 || p[5] < 0 || p[5] >= classes || floorf(p[5]) != p[5])
    return;
  Proposal box{
      Clamp(p[0], 0, width),
      Clamp(p[1], 0, height),
      Clamp(p[2], 0, width),
      Clamp(p[3], 0, height),
      p[4],
      static_cast<int>(p[5]),
      i};
  if (box.right - box.left < 1 || box.bottom - box.top < 1)
    return;
  proposals[atomicAdd(count, 1u)] = box;
}
} // namespace

cudaError_t PreprocessDetector(const Image& image, const Letterbox& box, float* tensor, cudaStream_t stream) {
  if (!gpu::ValidateImage(image) || !tensor || box.width <= 0 || box.height <= 0 || box.resized_width <= 0 ||
      box.resized_height <= 0 || box.left < 0 || box.top < 0 || box.left > box.width - box.resized_width ||
      box.top > box.height - box.resized_height || !std::isfinite(box.scale) || box.scale <= 0 ||
      (image.format != PixelFormat::kRgba8 && image.format != PixelFormat::kRgb10A2 &&
       image.format != PixelFormat::kNv12 && image.format != PixelFormat::kP010 &&
       image.format != PixelFormat::kRgba16Float) ||
      static_cast<size_t>(box.width) > std::numeric_limits<size_t>::max() / box.height / (3 * sizeof(float)))
    return cudaErrorInvalidValue;
  const dim3 block(16, 16),
      grid((static_cast<unsigned>(box.width) + 15) / 16, (static_cast<unsigned>(box.height) + 15) / 16);
  Preprocess<<<grid, block, 0, stream>>>(image, box, tensor);
  return cudaGetLastError();
}

cudaError_t FilterDetectorProposals(
    const float* predictions,
    uint32_t rows,
    int classes,
    float threshold,
    int network_width,
    int network_height,
    Proposal* proposals,
    uint32_t* count,
    cudaStream_t stream) {
  if (!predictions || !rows || rows > std::numeric_limits<uint32_t>::max() - 255 || classes <= 0 ||
      !std::isfinite(threshold) || threshold < 0 || threshold > 1 || network_width <= 0 || network_height <= 0 ||
      !proposals || !count)
    return cudaErrorInvalidValue;
  auto status = cudaMemsetAsync(count, 0, sizeof(*count), stream);
  if (status != cudaSuccess)
    return status;
  Filter<<<(rows + 255) / 256, 256, 0, stream>>>(
      predictions, rows, classes, threshold, network_width, network_height, proposals, count);
  return cudaGetLastError();
}
} // namespace hm::detection
