#include "hstream/src/libs/gpu/ConvertImage.h"
#include "hstream/src/libs/gpu/ImageSampling.cuh"

namespace hm::gpu {
namespace {
__global__ void Convert(Image input, void* output, size_t pitch) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= input.width || y >= input.height)
    return;
  const auto p = Pixel(input, x, y);
  auto* destination = reinterpret_cast<__half*>(static_cast<uint8_t*>(output) + y * pitch) + x * 4;
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
cudaError_t ConvertToRgba16Float(const Image& input, void* output, size_t pitch, cudaStream_t stream) {
  const Image destination{output, nullptr, pitch, 0, input.width, input.height, PixelFormat::kRgba16Float};
  if (!ValidateImage(input) || !ValidateImage(destination))
    return cudaErrorInvalidValue;
  const size_t output_bytes = pitch * input.height;
  if (Overlap(input.plane0, input.pitch0 * input.height, output, output_bytes) ||
      ((input.format == PixelFormat::kNv12 || input.format == PixelFormat::kP010) &&
       Overlap(input.plane1, input.pitch1 * (input.height / 2), output, output_bytes)))
    return cudaErrorInvalidValue;
  const dim3 block(16, 16), grid((input.width + 15) / 16, (input.height + 15) / 16);
  Convert<<<grid, block, 0, stream>>>(input, output, pitch);
  return cudaGetLastError();
}
} // namespace hm::gpu
