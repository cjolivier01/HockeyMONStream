#pragma once

#include <cuda_runtime_api.h>

namespace hm::gpu_preview {

cudaError_t WritePreviewTexture(
    cudaSurfaceObject_t destination,
    const void* rgba,
    size_t pitch,
    unsigned width,
    unsigned height,
    cudaStream_t stream);

} // namespace hm::gpu_preview
