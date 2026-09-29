#pragma once

#include <cuda_runtime.h>

#include <cstddef>

namespace hm::scoreboard {

// Applies a small Gaussian unsharp mask to only the visible scoreboard rectangle.
// Source and destination may have different pitches and must not overlap.
cudaError_t sharpen_scoreboard(
    const uchar4* source,
    size_t source_pitch,
    uchar4* destination,
    size_t destination_pitch,
    int width,
    int height,
    float amount,
    cudaStream_t stream);

} // namespace hm::scoreboard
