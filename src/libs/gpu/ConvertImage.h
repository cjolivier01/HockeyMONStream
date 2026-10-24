#pragma once

#include "hstream/src/libs/gpu/Image.h"
#include "hstream/src/libs/gpu/Runtime.h"

namespace hm::gpu {
// Convert every input pixel, including the final row/column, to packed IEEE
// binary16 RGBA in panorama units (0..255). The output has the input dimensions;
// row padding is untouched. Input/output allocations must be disjoint, and
// remain alive until stream completion even after a submission error. No
// allocation, synchronization, or video readback. Colorimetry is supplied in
// the input descriptor; this is not a transfer-function/HDR conversion.
cudaError_t ConvertToRgba16Float(const Image& input, void* output, size_t pitch, cudaStream_t stream);
} // namespace hm::gpu
