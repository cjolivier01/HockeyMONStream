#pragma once

#include <cuda_runtime.h>

// #include "deepstream/sources/includes/nvbufsurface.h"
#include "nvbufsurface.h"
#include "hockeymom/csrc/play_tracker/BoxUtils.h"

namespace hm {
namespace playcropper {

cudaError_t combinedTransform(
    const NvBufSurfaceParams* in_params,
    const hm::BBox& src_rect,
    float angle,
    const hm::Point& anchor_point,
    const hm::BBox& crop_box,
    NvBufSurfaceParams* out_params,
    const hm::BBox& output_rect,
    cudaStream_t stream);

}
}
