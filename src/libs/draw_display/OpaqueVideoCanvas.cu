#include "hstream/src/libs/draw_display/OpaqueVideoCanvas.h"

#include <limits>

namespace hm::draw_display {
namespace {
__global__ void Finish(analytics::ImageView output, VideoRectangle video) {
  const unsigned x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= output.width || y >= output.height)
    return;
  auto* pixel = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(output.data) + y * output.pitch) + x;
  const bool inside = x >= video.left && x - video.left < video.width && y >= video.top && y - video.top < video.height;
  *pixel = inside ? (*pixel | 0xff000000U) : 0xff000000U;
}
} // namespace

cudaError_t FinishOpaqueVideoCanvas(const analytics::ImageView& output, VideoRectangle video, cudaStream_t stream) {
  if (output.format != analytics::PixelFormat::kRgba8 || !output.data || !output.width || !output.height ||
      output.width > 16384 || output.height > 16384 || output.pitch < size_t(output.width) * 4 || output.pitch % 4 ||
      reinterpret_cast<uintptr_t>(output.data) % 4 ||
      output.pitch > std::numeric_limits<size_t>::max() / output.height || !video.width || !video.height ||
      video.left > output.width || video.width > output.width - video.left || video.top > output.height ||
      video.height > output.height - video.top)
    return cudaErrorInvalidValue;
  Finish<<<dim3((output.width + 31) / 32, (output.height + 7) / 8), dim3(32, 8), 0, stream>>>(output, video);
  return cudaGetLastError();
}
} // namespace hm::draw_display
