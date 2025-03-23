#include "hstream/src/libs/draw_display/DrawDisplayMeta.h"
#include "deepstream/sources/includes/nvll_osd_struct.h"
#include "hstream/src/libs/common/Status.h"
#include "jetson-utils/cuda/cudaDraw.h"

namespace hm {
namespace draw_display {

namespace {

const float4 no_color = make_float4(0, 0, 0, 0);

template<typename T> inline __device__ __host__ T sqr(T x) 				    { return x*x; }

inline float dist2(float x1, float y1, float x2, float y2) { return sqr(x1-x2) + sqr(y1-y2); }
inline float dist(float x1, float y1, float x2, float y2)  { return sqrtf(dist2(x1,y1,x2,y2)); }

// Helper to get the background color from a meta struct.
template <typename T>
inline float4 bg_color(const T& meta) {
  return meta.has_bg_color
      ? make_float4(meta.bg_color.red, meta.bg_color.green, meta.bg_color.blue, meta.bg_color.alpha)
      : no_color;
}

constexpr inline float4 fix_color(const float4& clr) {
  return {
      std::clamp(clr.x * 255, 0.0f, 255.0f),
      std::clamp(clr.y * 255, 0.0f, 255.0f),
      std::clamp(clr.z * 255, 0.0f, 255.0f),
      std::clamp(clr.w * 255, 0.0f, 255.0f)};
}

} // namespace

// Overloaded helper for rectangles. This version takes an NvOSD_RectParams struct.
cudaError_t cudaDraw(
    void* image,
    size_t width,
    size_t height,
    imageFormat format,
    const NvOSD_RectParams& rect,
    float scale,
    cudaStream_t stream) {
  int left = scale * rect.left;
  int top = scale * rect.top;
  int right = scale * (rect.left + rect.width);
  int bottom = scale * (rect.top + rect.height);
  float4 fillColor = bg_color(rect);
  float4 borderColor =
      make_float4(rect.border_color.red, rect.border_color.green, rect.border_color.blue, rect.border_color.alpha);
  float lineWidth = rect.border_width;
  return ::cudaDrawRect(
      image,
      width,
      height,
      format,
      left,
      top,
      right,
      bottom,
      fix_color(fillColor),
      fix_color(borderColor),
      std::max(scale * lineWidth, 1.0f),
      stream);
}

// Overloaded helper for circles. This version takes an NvOSD_CircleParams struct.
cudaError_t cudaDraw(
    void* image,
    size_t width,
    size_t height,
    imageFormat format,
    const NvOSD_CircleParams& circle,
    float scale,
    cudaStream_t stream) {
  int cx = scale * circle.xc;
  int cy = scale * circle.yc;
  float radius = scale * static_cast<float>(circle.radius);
  float4 color = make_float4(
      circle.circle_color.red, circle.circle_color.green, circle.circle_color.blue, circle.circle_color.alpha);
  float inner_radius = radius - circle.circle_width; 
  if (inner_radius < 0) {
    inner_radius = 0;
  }
  return ::cudaDrawCircle(image, width, height, format, cx, cy, radius, inner_radius, fix_color(color), stream);
}

// Overloaded helper for lines. This version takes an NvOSD_LineParams struct.
cudaError_t cudaDraw(
    void* image,
    size_t width,
    size_t height,
    imageFormat format,
    const NvOSD_LineParams& line,
    float scale,
    cudaStream_t stream) {
  int x1 = scale * line.x1;
  int y1 = scale * line.y1;
  int x2 = scale * line.x2;
  int y2 = scale * line.y2;
  if (dist(x1, y1, x2, y2) < 2.0) {
    return cudaError_t::cudaSuccess;
  }
  float4 color = make_float4(line.line_color.red, line.line_color.green, line.line_color.blue, line.line_color.alpha);
  float lineWidth = line.line_width;
  cudaError_t cuerr = ::cudaDrawLine(
      image, width, height, format, x1, y1, x2, y2, fix_color(color), std::max(scale * lineWidth, 1.0f), stream);
  return cuerr;
}

// absl::Status draw_display_meta(
//     surface::Surface surface,
//     const NvDsDisplayMeta* display_meta,
//     std::shared_ptr<FontCache> font_cache,
//     float scale,
//     cudaStream_t stream) {
//   // For now, we assume no extra pitch.
//   assert(surface.pitch_width() == surface.width());

//   const imageFormat format = surface.get_image_format();
//   const int ww = surface.width();
//   const int hh = surface.height();

//   // Draw each rectangle.
//   for (size_t i = 0; i < display_meta->num_rects; ++i) {
//     const NvOSD_RectParams& rect = display_meta->rect_params[i];
//     XCUDA_RETURN_IF_ERROR(cudaDraw(surface.dataptr(), ww, hh, format, rect, scale, stream));
//   }
//   // Draw each circle.
//   for (size_t i = 0; i < display_meta->num_circles; ++i) {
//     const NvOSD_CircleParams& circle = display_meta->circle_params[i];
//     XCUDA_RETURN_IF_ERROR(cudaDraw(surface.dataptr(), ww, hh, format, circle, scale, stream));
//   }
//   // Draw each line.
//   for (size_t i = 0; i < display_meta->num_lines; ++i) {
//     const NvOSD_LineParams& line = display_meta->line_params[i];
//     XCUDA_RETURN_IF_ERROR(cudaDraw(surface.dataptr(), ww, hh, format, line, scale, stream));
//   }
//   for (size_t i = 0; i < display_meta->num_labels; ++i) {
//     const NvOSD_TextParams& text = display_meta->text_params[i];
//     if (!text.display_text || !*text.display_text) {
//       continue;
//     }
//     std::shared_ptr<Font> font;
//     HM_ASSIGN_OR_RETURN(font, font_cache->get_or_create_font(text.font_params.font_name, text.font_params.font_size));
//     std::pair<int, int> newpos;
//     HM_ASSIGN_OR_RETURN(
//         newpos,
//         font->draw(
//             text.display_text,
//             surface.dataptr(),
//             surface.width(),
//             surface.height(),
//             text.x_offset,
//             text.y_offset,
//             uchar4{
//                 (uint8_t)text.font_params.font_color.red,
//                 (uint8_t)text.font_params.font_color.green,
//                 (uint8_t)text.font_params.font_color.blue,
//                 (uint8_t)text.font_params.font_color.alpha}));
//     (void)newpos;
//   }
//   return absl::OkStatus();
// }

} // namespace draw_display
} // namespace hm
