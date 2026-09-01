#pragma once

#include <cuda_runtime.h>

// #include "deepstream/sources/includes/nvbufsurface.h"
#include "hockeymon/csrc/play_tracker/BoxUtils.h"
#include "nvbufsurface.h"

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
    float shadow_lift_percent,
    bool lift_shadow_black_point,
    float exposure,
    cudaStream_t stream);

}
} // namespace hm
