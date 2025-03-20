#pragma once

#include "hstream/src/libs/common/Surface.h"
#include "hstream/src/libs/draw_display/Fonts.h"

#include <cuda_runtime.h>

#include "deepstream/sources/includes/nvdsmeta.h"

#include "absl/status/status.h"

namespace hm {
namespace draw_display {

absl::Status draw_display_meta(
    surface::Surface surface,
    const NvDsDisplayMeta* display_meta,
    std::shared_ptr<FontCache> font_cache,
    cudaStream_t stream);

cudaError_t cudaDraw(
    void* image,
    size_t width,
    size_t height,
    imageFormat format,
    const NvOSD_RectParams& rect,
    cudaStream_t stream = 0);

// Overloaded helper for circles. This version takes an NvOSD_CircleParams struct.
cudaError_t cudaDraw(
    void* image,
    size_t width,
    size_t height,
    imageFormat format,
    const NvOSD_CircleParams& circle,
    cudaStream_t stream = 0);

// Overloaded helper for lines. This version takes an NvOSD_LineParams struct.
cudaError_t cudaDraw(
    void* image,
    size_t width,
    size_t height,
    imageFormat format,
    const NvOSD_LineParams& line,
    cudaStream_t stream = 0);

} // namespace draw_display
} // namespace hm
