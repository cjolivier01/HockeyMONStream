#include "hstream/src/libs/draw_display/DrawDisplayMeta.h"
#include "hstream/src/libs/common/Status.h"
#include "jetson-utils/cuda/cudaDraw.h"

namespace hm {
namespace draw_display {

namespace {

// inline cudaError_t cudaDrawRect( void* image, size_t width, size_t height, imageFormat format,
//                                  int left, int top, int right, int bottom, const float4& color,
//                                  const float4& line_color=make_float4(0,0,0,0), float line_width=1.0f,
//                                  cudaStream_t stream=0 )

// inline cudaError_t cudaDrawCircle( void* image, size_t width, size_t height, imageFormat format,
//                                    int cx, int cy, float radius, const float4& color, cudaStream_t stream=0 )

const float4 no_color = make_float4(0, 0, 0, 0);

template <typename T>
inline float4 bg_color(const T& meta) {
  return meta.has_bg_color
      ? make_float4(meta.bg_color.red, meta.bg_color.green, meta.bg_color.blue, meta.bg_color.alpha)
      : no_color;
}

} // namespace

absl::Status draw_display_meta(surface::Surface surface, const NvDsDisplayMeta* display_meta, cudaStream_t stream) {
  // Restriction for now, no extra pitch
  assert(surface.pitch_width() == surface.width());
  // Rectangles
  const imageFormat format = surface.get_image_format();
  const int ww = surface.width();
  const int hh = surface.height();

  for (size_t i = 0; i < display_meta->num_rects; ++i) {
    const NvOSD_RectParams& rect = display_meta->rect_params[i];
    XCUDA_RETURN_IF_ERROR(cudaDrawRect(
        surface.dataptr(),
        ww,
        hh,
        format,
        rect.left,
        rect.top,
        rect.left + rect.width,
        rect.top + rect.height,
        bg_color(rect),
        make_float4(rect.border_color.red, rect.border_color.green, rect.border_color.blue, rect.border_color.alpha),
        /*line_width=*/1,
        stream));
  }
  for (size_t i = 0; i < display_meta->num_circles; ++i) {
    // TODO: Need a kernel to do outer line and backgrouns and visa versa
    const NvOSD_CircleParams& circle = display_meta->circle_params[i];
    XCUDA_RETURN_IF_ERROR(cudaDrawCircle(
        surface.dataptr(),
        ww,
        hh,
        format,
        circle.xc,
        circle.yc,
        float(circle.radius),
        make_float4(
            circle.circle_color.red, circle.circle_color.green, circle.circle_color.blue, circle.circle_color.alpha),
        stream));
  }
  return absl::OkStatus();
}

} // namespace draw_display
} // namespace hm
