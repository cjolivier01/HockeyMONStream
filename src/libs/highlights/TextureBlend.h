#pragma once
#include <cuda_runtime_api.h>
#include <cstddef>
namespace hm::highlights {
// Texture assets are RGBA8. Video remains in device memory, in RGBA8 or RGB10A2.
cudaError_t BlendTexture(
    void* image,
    size_t pitch,
    int width,
    int height,
    bool ten_bit,
    const void* texture,
    int tw,
    int th,
    int x,
    int y,
    int w,
    int h,
    cudaStream_t stream);
cudaError_t ClearFrame(void* image, size_t pitch, int width, int height, bool ten_bit, cudaStream_t stream);
} // namespace hm::highlights
