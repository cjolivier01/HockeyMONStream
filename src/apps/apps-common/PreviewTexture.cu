#include "PreviewTexture.cuh"

#include <cuda_runtime.h>

namespace hm::gpu_preview {
namespace {

__global__ void write_rgba(
    cudaSurfaceObject_t destination, const unsigned char* rgba, size_t pitch, unsigned width, unsigned height) {
  const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
  const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x < width && y < height) {
    const auto* row = reinterpret_cast<const uchar4*>(rgba + y * pitch);
    surf2Dwrite(row[x], destination, x * sizeof(uchar4), y);
  }
}

} // namespace

cudaError_t WritePreviewTexture(
    cudaSurfaceObject_t destination,
    const void* rgba,
    size_t pitch,
    unsigned width,
    unsigned height,
    cudaStream_t stream) {
  const dim3 block(32, 8);
  const dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
  write_rgba<<<grid, block, 0, stream>>>(destination, static_cast<const unsigned char*>(rgba), pitch, width, height);
  return cudaGetLastError();
}

} // namespace hm::gpu_preview
