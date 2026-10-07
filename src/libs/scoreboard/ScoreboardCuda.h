#pragma once

#include <cuda_runtime.h>

#include <cstddef>

namespace hm::scoreboard {

// Video surfaces may carry zero alpha even when their RGB pixels are valid.
// Make the extracted scoreboard opaque before warping it, so the warp's
// transparent border remains available to the final overlay.
cudaError_t make_scoreboard_opaque(uchar4* pixels, size_t pitch, int width, int height, cudaStream_t stream);

} // namespace hm::scoreboard
