#pragma once

#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/Surface.h"
#include "hstream/src/libs/draw_display/Fonts.h"
#include "jetson-utils/cuda/cudaDraw.h"
#include "jetson-utils/cuda/cudaFont.h"

#include <cuda_runtime.h>

#include "deepstream/sources/includes/nvdsmeta.h"

#include "absl/status/status.h"

namespace hm {
namespace draw_display {

// absl::Status draw_display_meta(
//     surface::Surface surface,
//     const DISPLAY_META_T* display_meta,
//     std::shared_ptr<FontCache> font_cache,
//     float scale,
//     cudaStream_t stream);

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

template <typename DISPLAY_META_T>
inline absl::Status draw_display_meta(
    surface::Surface surface,
    const DISPLAY_META_T* display_meta,
    std::shared_ptr<FontCache> font_cache,
    float scale,
    cudaStream_t stream) {
  // For now, we assume no extra pitch.
  assert(surface.pitch_width() == surface.width());

  const imageFormat format = surface.get_image_format();
  const int ww = surface.width();
  const int hh = surface.height();

  // Draw each rectangle.
  for (size_t i = 0; i < display_meta->num_rects; ++i) {
    const NvOSD_RectParams& rect = display_meta->rect_params[i];
    XCUDA_RETURN_IF_ERROR(cudaDraw(surface.dataptr(), ww, hh, format, rect, scale, stream));
  }
  // Draw each circle.
  for (size_t i = 0; i < display_meta->num_circles; ++i) {
    const NvOSD_CircleParams& circle = display_meta->circle_params[i];
    XCUDA_RETURN_IF_ERROR(cudaDraw(surface.dataptr(), ww, hh, format, circle, scale, stream));
  }
  // Draw each line.
  for (size_t i = 0; i < display_meta->num_lines; ++i) {
    const NvOSD_LineParams& line = display_meta->line_params[i];
    XCUDA_RETURN_IF_ERROR(cudaDraw(surface.dataptr(), ww, hh, format, line, scale, stream));
  }
  for (size_t i = 0; i < display_meta->num_labels; ++i) {
    const NvOSD_TextParams& text = display_meta->text_params[i];
    if (!text.display_text || !*text.display_text) {
      continue;
    }
    std::shared_ptr<Font> font;
    HM_ASSIGN_OR_RETURN(font, font_cache->get_or_create_font(text.font_params.font_name, text.font_params.font_size));
    std::pair<int, int> newpos;
    HM_ASSIGN_OR_RETURN(
        newpos,
        font->draw(
            text.display_text,
            surface.dataptr(),
            surface.width(),
            surface.height(),
            text.x_offset,
            text.y_offset,
            uchar4{
                (uint8_t)text.font_params.font_color.red,
                (uint8_t)text.font_params.font_color.green,
                (uint8_t)text.font_params.font_color.blue,
                (uint8_t)text.font_params.font_color.alpha}));
    (void)newpos;
  }
  return absl::OkStatus();
}

} // namespace draw_display
} // namespace hm
