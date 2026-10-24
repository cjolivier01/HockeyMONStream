#pragma once

#include "hstream/src/libs/gpu/Runtime.h"

#include <cstddef>
#include <cstdint>

#include "hstream/src/libs/detection/DetectionTypes.h"

namespace hm::detection {

enum class PixelFormat { kRgba8, kRgb10A2, kNv12, kP010 };

// Borrowed GPU planes. YUV inputs require explicit range/matrix coefficients:
// rgb = y_scale * (Y - y_offset) + chroma coefficients * (U/V - uv_offset).
// Components are in 8-bit video units, including P010 scaled by 255/1023.
struct Image {
  const void* plane0;
  const void* plane1;
  size_t pitch0, pitch1;
  int width, height;
  PixelFormat format;
  float y_offset, uv_offset, y_scale, r_v, g_u, g_v, b_u;
};

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
