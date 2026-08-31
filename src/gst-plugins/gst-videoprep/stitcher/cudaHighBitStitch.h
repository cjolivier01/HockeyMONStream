#pragma once

#include <cuda_runtime.h>

#include "cupano/cuda/cudaTypes.h"
#include "nvbufsurface.h"

namespace hm::stitcher {

bool isRgb10A2ColorFormat(NvBufSurfaceColorFormat format);

cudaError_t unpackRgb10A2ToHalf4(
    const NvBufSurfaceParams* input,
    half4* output,
    size_t output_pitch,
    cudaStream_t stream);

cudaError_t convertHalf4ToRgba8(
    const half4* input,
    size_t input_pitch,
    int input_width,
    int input_height,
    NvBufSurfaceParams* output,
    double rotation_degrees,
    float shadow_lift_percent,
    bool lift_shadow_black_point,
    float exposure,
    cudaStream_t stream);

cudaError_t convertHalf4ToBgr10A2(
    const half4* input,
    size_t input_pitch,
    int input_width,
    int input_height,
    NvBufSurfaceParams* output,
    double rotation_degrees,
    float shadow_lift_percent,
    bool lift_shadow_black_point,
    float exposure,
    cudaStream_t stream);

// Calibration must observe the stitched image before user-facing tone controls.
cudaError_t convertHalf4ToCalibrationRgba8(
    const half4* input,
    size_t input_pitch,
    int input_width,
    int input_height,
    NvBufSurfaceParams* output,
    double rotation_degrees,
    cudaStream_t stream);

} // namespace hm::stitcher
