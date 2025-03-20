#pragma once

#include <cuda_runtime.h>

namespace hm {
namespace draw_display {

cudaError_t drawGlyph(
    uchar4* d_img,
    int imgWidth,
    int imgHeight,
    const unsigned char* d_glyph,
    int glyphWidth,
    int glyphHeight,
    int destX,
    int destY,
    uchar4 textColor,
    float threshold,
    cudaStream_t stream);

}
} // namespace hm
