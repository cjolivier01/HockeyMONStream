#include "hstream/src/libs/gpu/ConvertImage.h"
#include "hstream/src/libs/gpu/ImageSampling.cuh"

namespace hm::gpu {
namespace {
__global__ void Convert(Image input, void* output, size_t pitch, media::DisplayTransform display) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= input.width || y >= input.height)
    return;
  const int out_x = display.transpose ? y : x, out_y = display.transpose ? x : y;
  const bool mirror_x = display.transpose ? display.flip.vertical : display.flip.horizontal;
  const bool mirror_y = display.transpose ? display.flip.horizontal : display.flip.vertical;
  const auto p = Pixel(input, mirror_x ? input.width - 1 - x : x, mirror_y ? input.height - 1 - y : y);
  auto* destination = reinterpret_cast<__half*>(static_cast<uint8_t*>(output) + out_y * pitch) + out_x * 4;
  destination[0] = __float2half_rn(p.x);
  destination[1] = __float2half_rn(p.y);
  destination[2] = __float2half_rn(p.z);
  destination[3] = __float2half_rn(p.w);
}
bool Overlap(const void* a, size_t a_size, const void* b, size_t b_size) {
  const auto a_start = reinterpret_cast<uintptr_t>(a), b_start = reinterpret_cast<uintptr_t>(b);
  return a_start < b_start + b_size && b_start < a_start + a_size;
}
} // namespace
cudaError_t ConvertToRgba16Float(
    const Image& input,
    void* output,
    size_t pitch,
    cudaStream_t stream,
    media::DisplayTransform display) {
  const Image destination{
      output,
      nullptr,
      pitch,
      0,
      display.transpose ? input.height : input.width,
      display.transpose ? input.width : input.height,
      PixelFormat::kRgba16Float};
  if (!ValidateImage(input) || !ValidateImage(destination))
    return cudaErrorInvalidValue;
  const size_t output_bytes = pitch * destination.height;
  if (Overlap(input.plane0, input.pitch0 * input.height, output, output_bytes) ||
      ((input.format == PixelFormat::kNv12 || input.format == PixelFormat::kP010) &&
       Overlap(input.plane1, input.pitch1 * (input.height / 2), output, output_bytes)))
    return cudaErrorInvalidValue;
  const dim3 block(16, 16), grid((input.width + 15) / 16, (input.height + 15) / 16);
  Convert<<<grid, block, 0, stream>>>(input, output, pitch, display);
  return cudaGetLastError();
}
} // namespace hm::gpu
