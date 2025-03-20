#include "hstream/src/libs/draw_display/DrawDisplayMeta.h"
#include "hstream/src/libs/common/Status.h"
#include "jetson-utils/cuda/cudaDraw.h"

namespace hm {
namespace draw_display {

namespace {

// Original function prototypes (from jetson-utils/cuda/cudaDraw.h):
//
// inline cudaError_t cudaDrawRect( void* image, size_t width, size_t height, imageFormat format,
//                                  int left, int top, int right, int bottom, const float4& color,
//                                  const float4& line_color = make_float4(0,0,0,0), float line_width = 1.0f,
//                                  cudaStream_t stream = 0 )
//
// inline cudaError_t cudaDrawCircle( void* image, size_t width, size_t height, imageFormat format,
//                                    int cx, int cy, float radius, const float4& color, cudaStream_t stream = 0 )
//
// cudaError_t cudaDrawLine( void* image, size_t width, size_t height, imageFormat format,
//                           int x1, int y1, int x2, int y2, const float4& color, float line_width = 1.0,
//                           cudaStream_t stream = 0 );

const float4 no_color = make_float4(0, 0, 0, 0);

// Helper to get the background color from a meta struct.
template <typename T>
inline float4 bg_color(const T& meta) {
  return meta.has_bg_color
      ? make_float4(meta.bg_color.red, meta.bg_color.green, meta.bg_color.blue, meta.bg_color.alpha)
      : no_color;
}
} // namespace

// Overloaded helper for rectangles. This version takes an NvOSD_RectParams struct.
cudaError_t cudaDraw(
    void* image,
    size_t width,
    size_t height,
    imageFormat format,
    const NvOSD_RectParams& rect,
    cudaStream_t stream) {
  int left = rect.left;
  int top = rect.top;
  int right = rect.left + rect.width;
  int bottom = rect.top + rect.height;
  float4 fillColor = bg_color(rect);
  float4 borderColor =
      make_float4(rect.border_color.red, rect.border_color.green, rect.border_color.blue, rect.border_color.alpha);
  float lineWidth = rect.border_width;
  return ::cudaDrawRect(
      image, width, height, format, left, top, right, bottom, fillColor, borderColor, lineWidth, stream);
}

// Overloaded helper for circles. This version takes an NvOSD_CircleParams struct.
cudaError_t cudaDraw(
    void* image,
    size_t width,
    size_t height,
    imageFormat format,
    const NvOSD_CircleParams& circle,
    cudaStream_t stream) {
  int cx = circle.xc;
  int cy = circle.yc;
  float radius = static_cast<float>(circle.radius);
  float4 color = make_float4(
      circle.circle_color.red, circle.circle_color.green, circle.circle_color.blue, circle.circle_color.alpha);
  return ::cudaDrawCircle(image, width, height, format, cx, cy, radius, color, stream);
}

// Overloaded helper for lines. This version takes an NvOSD_LineParams struct.
cudaError_t cudaDraw(
    void* image,
    size_t width,
    size_t height,
    imageFormat format,
    const NvOSD_LineParams& line,
    cudaStream_t stream) {
  int x1 = line.x1;
  int y1 = line.y1;
  int x2 = line.x2;
  int y2 = line.y2;
  float4 color = make_float4(line.line_color.red, line.line_color.green, line.line_color.blue, line.line_color.alpha);
  float lineWidth = line.line_width;
  return ::cudaDrawLine(image, width, height, format, x1, y1, x2, y2, color, lineWidth, stream);
}

absl::Status draw_display_meta(surface::Surface surface, const NvDsDisplayMeta* display_meta, cudaStream_t stream) {
  // For now, we assume no extra pitch.
  assert(surface.pitch_width() == surface.width());

  const imageFormat format = surface.get_image_format();
  const int ww = surface.width();
  const int hh = surface.height();

  // Draw each rectangle.
  for (size_t i = 0; i < display_meta->num_rects; ++i) {
    const NvOSD_RectParams& rect = display_meta->rect_params[i];
    XCUDA_RETURN_IF_ERROR(cudaDraw(surface.dataptr(), ww, hh, format, rect, stream));
  }
  // Draw each circle.
  for (size_t i = 0; i < display_meta->num_circles; ++i) {
    const NvOSD_CircleParams& circle = display_meta->circle_params[i];
    XCUDA_RETURN_IF_ERROR(cudaDraw(surface.dataptr(), ww, hh, format, circle, stream));
  }
  // Draw each line.
  for (size_t i = 0; i < display_meta->num_lines; ++i) {
    const NvOSD_LineParams& line = display_meta->line_params[i];
    XCUDA_RETURN_IF_ERROR(cudaDraw(surface.dataptr(), ww, hh, format, line, stream));
  }
  return absl::OkStatus();
}

} // namespace draw_display
} // namespace hm
