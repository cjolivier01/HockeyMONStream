#include "hstream/src/libs/detection/DetectorGpu.h"

#include <cmath>
#include <limits>

namespace hm::detection {
namespace {
__device__ float Clamp(float value, float low, float high) {
  return fminf(fmaxf(value, low), high);
}
__device__ float3 Pixel(const Image& image, int x, int y) {
  const auto* row = static_cast<const uint8_t*>(image.plane0) + static_cast<size_t>(y) * image.pitch0;
  if (image.format == PixelFormat::kRgba8) {
    const auto p = reinterpret_cast<const uchar4*>(row)[x];
    return make_float3(p.x, p.y, p.z);
  }
  if (image.format == PixelFormat::kRgb10A2) {
    const auto p = reinterpret_cast<const uint32_t*>(row)[x];
    return make_float3(
        (p & 1023) * (255.f / 1023), ((p >> 10) & 1023) * (255.f / 1023), ((p >> 20) & 1023) * (255.f / 1023));
  }
  const auto* chroma = static_cast<const uint8_t*>(image.plane1) + static_cast<size_t>(y / 2) * image.pitch1;
  float luma, u, v;
  if (image.format == PixelFormat::kNv12) {
    luma = row[x];
    u = chroma[(x / 2) * 2];
    v = chroma[(x / 2) * 2 + 1];
  } else {
    luma = (reinterpret_cast<const uint16_t*>(row)[x] >> 6) * (255.f / 1023);
    u = (reinterpret_cast<const uint16_t*>(chroma)[(x / 2) * 2] >> 6) * (255.f / 1023);
    v = (reinterpret_cast<const uint16_t*>(chroma)[(x / 2) * 2 + 1] >> 6) * (255.f / 1023);
  }
  luma = (luma - image.y_offset) * image.y_scale;
  u -= image.uv_offset;
  v -= image.uv_offset;
  return make_float3(
      Clamp(luma + image.r_v * v, 0, 255),
      Clamp(luma + image.g_u * u + image.g_v * v, 0, 255),
      Clamp(luma + image.b_u * u, 0, 255));
}
__device__ float Mix(float a, float b, float c, float d, float dx, float dy) {
  return (a * (1 - dx) + b * dx) * (1 - dy) + (c * (1 - dx) + d * dx) * dy;
}
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
bool FiniteColor(const Image& image) {
  return std::isfinite(image.y_offset) && std::isfinite(image.uv_offset) && std::isfinite(image.y_scale) &&
      std::isfinite(image.r_v) && std::isfinite(image.g_u) && std::isfinite(image.g_v) && std::isfinite(image.b_u);
}
} // namespace

cudaError_t PreprocessDetector(const Image& image, const Letterbox& box, float* tensor, cudaStream_t stream) {
  const bool yuv = image.format == PixelFormat::kNv12 || image.format == PixelFormat::kP010;
  const size_t bytes = image.format == PixelFormat::kNv12 ? 1 : image.format == PixelFormat::kP010 ? 2 : 4;
  if (!image.plane0 || !tensor || image.width <= 0 || image.height <= 0 || box.width <= 0 || box.height <= 0 ||
      image.width >= (1 << 24) || image.height >= (1 << 24) || box.resized_width <= 0 || box.resized_height <= 0 ||
      box.left < 0 || box.top < 0 || box.left > box.width - box.resized_width ||
      box.top > box.height - box.resized_height || !std::isfinite(box.scale) || box.scale <= 0 ||
      (!yuv && image.format != PixelFormat::kRgba8 && image.format != PixelFormat::kRgb10A2) ||
      image.pitch0 < static_cast<size_t>(image.width) * bytes || image.pitch0 % bytes ||
      reinterpret_cast<uintptr_t>(image.plane0) % bytes ||
      image.pitch0 > std::numeric_limits<size_t>::max() / image.height ||
      static_cast<size_t>(box.width) > std::numeric_limits<size_t>::max() / box.height / (3 * sizeof(float)))
    return cudaErrorInvalidValue;
  if (yuv &&
      (!image.plane1 || image.width % 2 || image.height % 2 || !FiniteColor(image) ||
       image.pitch1 < static_cast<size_t>(image.width) * bytes || image.pitch1 % bytes ||
       reinterpret_cast<uintptr_t>(image.plane1) % bytes ||
       image.pitch1 > std::numeric_limits<size_t>::max() / (image.height / 2)))
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
