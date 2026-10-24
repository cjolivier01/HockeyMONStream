#pragma once

#include "hstream/src/libs/gpu/Image.h"
#include "hstream/src/libs/gpu/Runtime.h"

#include <cstddef>
#include <cstdint>

#include "hstream/src/libs/detection/DetectionTypes.h"

namespace hm::detection {

using gpu::Image;
using gpu::PixelFormat;

// Half-pixel bilinear sampling, edge clamp, black letterbox, RGB float32 NCHW.
// All parameters are explicit; the caller resolves the model configuration.
// Source dimensions must be below 2^24 for exact float pixel coordinates.
// No allocation, upload, synchronization, or video readback.
cudaError_t PreprocessDetector(const Image& image, const Letterbox& box, float* tensor, cudaStream_t stream);

// Compact valid [x1,y1,x2,y2,score,class] rows on the device. Output capacity is
// rows, so thresholding cannot silently discard a later, higher score. count
// is reset on the caller's stream. Only this compact metadata needs readback.
cudaError_t FilterDetectorProposals(
    const float* predictions,
    uint32_t rows,
    int classes,
    float threshold,
    int network_width,
    int network_height,
    Proposal* proposals,
    uint32_t* count,
    cudaStream_t stream);

} // namespace hm::detection
