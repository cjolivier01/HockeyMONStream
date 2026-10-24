#include "hstream/src/libs/highlights/TextureBlend.h"

#include <cstdint>
#include <limits>

namespace hm::highlights {
namespace {
__global__ void blend(
    unsigned char* image,
    size_t pitch,
    int iw,
    int ih,
    bool ten,
    const uchar4* tile,
    int tw,
    int th,
    int ox,
    int oy,
    int w,
    int h) {
  const int tx = blockIdx.x * blockDim.x + threadIdx.x, ty = blockIdx.y * blockDim.y + threadIdx.y;
  if (tx >= w || ty >= h)
    return;
  const int x = ox + tx, y = oy + ty;
  if (x < 0 || y < 0 || x >= iw || y >= ih)
    return;
  const float sx = (tx + 0.5f) * tw / w - 0.5f, sy = (ty + 0.5f) * th / h - 0.5f;
  const int x0 = floorf(sx), y0 = floorf(sy);
  const float fx = sx - x0, fy = sy - y0;
  float r = 0, g = 0, b = 0, a = 0;
  // Interpolate premultiplied colors to avoid dark fringes on transparent text.
  for (int j = 0; j < 2; ++j)
    for (int i = 0; i < 2; ++i) {
      const int px = max(0, min(tw - 1, x0 + i)), py = max(0, min(th - 1, y0 + j));
      const uchar4 p = tile[py * tw + px];
      const float weight = (i ? fx : 1 - fx) * (j ? fy : 1 - fy), alpha = p.w / 255.f;
      a += alpha * weight;
      r += p.x * alpha * weight;
      g += p.y * alpha * weight;
      b += p.z * alpha * weight;
    }
  if (a == 0)
    return;
  if (ten) {
    auto* target = reinterpret_cast<uint32_t*>(image + y * pitch) + x;
    const uint32_t old = *target;
    const uint32_t rr = min(1023u, uint32_t(((old & 1023) * (1 - a) + r * 1023 / 255) + 0.5f));
    const uint32_t gg = min(1023u, uint32_t((((old >> 10) & 1023) * (1 - a) + g * 1023 / 255) + 0.5f));
    const uint32_t bb = min(1023u, uint32_t((((old >> 20) & 1023) * (1 - a) + b * 1023 / 255) + 0.5f));
    *target = rr | (gg << 10) | (bb << 20) | 0xc0000000u;
  } else {
    auto* target = reinterpret_cast<uchar4*>(image + y * pitch) + x;
    const uchar4 old = *target;
    *target = make_uchar4(old.x * (1 - a) + r + 0.5f, old.y * (1 - a) + g + 0.5f, old.z * (1 - a) + b + 0.5f, 255);
  }
}
__global__ void clear(unsigned char* image, size_t pitch, int w, int h, bool ten) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x < w && y < h)
    reinterpret_cast<uint32_t*>(image + y * pitch)[x] = ten ? 0xc0000000u : 0xff000000u;
}
} // namespace
cudaError_t BlendTexture(
    void* image,
    size_t pitch,
    int width,
    int height,
    bool ten,
    const void* texture,
    int tw,
    int th,
    int x,
    int y,
    int w,
    int h,
    cudaStream_t stream) {
  if (!image || !texture || width <= 0 || height <= 0 || tw <= 0 || th <= 0 || w <= 0 || h <= 0 ||
      pitch < static_cast<size_t>(width) * 4 || pitch % alignof(uint32_t) != 0 ||
      static_cast<size_t>(height) > std::numeric_limits<size_t>::max() / pitch ||
      w > std::numeric_limits<int>::max() - 15 || h > std::numeric_limits<int>::max() - 15 ||
      static_cast<int64_t>(x) + w > std::numeric_limits<int>::max() ||
      static_cast<int64_t>(y) + h > std::numeric_limits<int>::max() ||
      static_cast<int64_t>(tw) * th > std::numeric_limits<int>::max())
    return cudaErrorInvalidValue;
  blend<<<dim3((w + 15) / 16, (h + 15) / 16), dim3(16, 16), 0, stream>>>(
      static_cast<unsigned char*>(image),
      pitch,
      width,
      height,
      ten,
      static_cast<const uchar4*>(texture),
      tw,
      th,
      x,
      y,
      w,
      h);
  return cudaGetLastError();
}
cudaError_t ClearFrame(void* image, size_t pitch, int w, int h, bool ten, cudaStream_t stream) {
  if (!image || w <= 0 || h <= 0 || pitch < static_cast<size_t>(w) * 4 || pitch % alignof(uint32_t) != 0 ||
      static_cast<size_t>(h) > std::numeric_limits<size_t>::max() / pitch || w > std::numeric_limits<int>::max() - 15 ||
      h > std::numeric_limits<int>::max() - 15)
    return cudaErrorInvalidValue;
  clear<<<dim3((w + 15) / 16, (h + 15) / 16), dim3(16, 16), 0, stream>>>(
      static_cast<unsigned char*>(image), pitch, w, h, ten);
  return cudaGetLastError();
}
} // namespace hm::highlights
