#include "src/libs/common/Surface.h"

#include <npp.h>

#include <cassert>

#include "cudaDraw.h"

namespace hm {
namespace {
imageFormat get_image_format(const surface::Surface& surface) {
  switch (surface->colorFormat) {
    case NVBUF_COLOR_FORMAT_RGBA:
      return imageFormat::IMAGE_RGBA8;
    case NVBUF_COLOR_FORMAT_NV12:
      return imageFormat::IMAGE_NV12;
    default:
      assert(false);
      return imageFormat::IMAGE_UNKNOWN;
  }
}

size_t get_pitch_width(const surface::Surface& surface) {
  switch (surface->colorFormat) {
    case NVBUF_COLOR_FORMAT_RGBA:
      return surface.pitch() * 4;
    case NVBUF_COLOR_FORMAT_NV12:
      return surface.width();
    default:
      assert(false);
      return 0;
  }
}

} // namespace

cudaError_t draw_rect(
    surface::Surface& surface,
    const hm::BBox& rect,
    const float4& color,
    int thickness,
    cudaStream_t stream,
    const std::optional<float4>& fill_color) {
  cudaError_t cu_err = cudaError_t::cudaSuccess;
  cu_err = cudaDrawRect(
      surface.dataptr(),
      surface.dataptr(),
      get_pitch_width(surface),
      surface.height(),
      get_image_format(surface),
      rect.left,
      rect.top,
      rect.right,
      rect.bottom,
      color,
      fill_color.has_value() ? *fill_color : make_float4(0, 0, 0, 0),
      thickness,
      stream);
  return cu_err;
}

} // namespace hm
