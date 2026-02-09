#pragma once

/* clang-format off */
#include "src/libs/common/Status.h"
/* clang-format on */

#include "hstream/src/libs/common/Surface.h"
#include "hstream/src/libs/draw_display/Fonts.h"

#include <cuda_runtime.h>

#include "nvdsmeta.h"

#include "absl/status/status.h"

namespace hm {
namespace draw_display {

absl::Status draw_display_meta(
    surface::Surface surface,
    const NvDsDisplayMeta* display_meta,
    std::shared_ptr<FontCache> font_cache,
    float scale,
    cudaStream_t stream);

absl::Status draw_object_meta(
    surface::Surface surface,
    std::vector<NvDsObjectMeta*>& object_meta,
    std::shared_ptr<FontCache> font_cache,
    float scale,
    cudaStream_t stream);

cudaError_t cudaDraw(
    void* image,
    size_t width,
    size_t height,
    imageFormat format,
    const NvOSD_RectParams& rect,
    float scale,
    cudaStream_t stream);

// Overloaded helper for circles. This version takes an NvOSD_CircleParams struct.
cudaError_t cudaDraw(
    void* image,
    size_t width,
    size_t height,
    imageFormat format,
    const NvOSD_CircleParams& circle,
    float scale,
    cudaStream_t stream);

// Overloaded helper for lines. This version takes an NvOSD_LineParams struct.
cudaError_t cudaDraw(
    void* image,
    size_t width,
    size_t height,
    imageFormat format,
    const NvOSD_LineParams& line,
    float scale,
    cudaStream_t stream);

} // namespace draw_display
} // namespace hm
