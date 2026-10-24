#include "hstream/src/gst-plugins/gst-videoprep/playcropper/cudaPlayCropper.h"
#include "hstream/src/libs/program/ProgramGpu.h"

#include <cmath>
#include <limits>

namespace hm::playcropper {
cudaError_t combinedTransform(
    const NvBufSurfaceParams* in_params,
    const hm::BBox& src_rect,
    float angle,
    const hm::Point& anchor_point,
    const hm::BBox& crop_box,
    NvBufSurfaceParams* out_params,
    const hm::BBox& output_rect,
    float shadow_lift_percent,
    bool lift_shadow_black_point,
    float exposure,
    cudaStream_t stream) {
  if (!in_params || !out_params || output_rect.left != 0 || output_rect.top != 0 || !std::isfinite(output_rect.right) ||
      !std::isfinite(output_rect.bottom) || output_rect.width() <= 0 || output_rect.height() <= 0 ||
      output_rect.width() > out_params->width || output_rect.height() > out_params->height ||
      output_rect.width() >= (1 << 24) || output_rect.height() >= (1 << 24) || in_params->width >= (1 << 24) ||
      in_params->height >= (1 << 24) || std::floor(output_rect.width()) != output_rect.width() ||
      std::floor(output_rect.height()) != output_rect.height() || in_params->colorFormat != out_params->colorFormat)
    return cudaErrorInvalidValue;
  gpu::PixelFormat format;
  switch (in_params->colorFormat) {
    case NVBUF_COLOR_FORMAT_RGBA:
      format = gpu::PixelFormat::kRgba8;
      break;
    case NVBUF_COLOR_FORMAT_RGB:
      format = gpu::PixelFormat::kRgb8;
      break;
    case NVBUF_COLOR_FORMAT_GRAY8:
      format = gpu::PixelFormat::kGray8;
      break;
    default:
      return cudaErrorInvalidValue;
  }
  const gpu::Image input{
      in_params->dataPtr,
      nullptr,
      in_params->pitch,
      0,
      static_cast<int>(in_params->width),
      static_cast<int>(in_params->height),
      format,
      0,
      0,
      0,
      0,
      0,
      0,
      0};
  const PackedOutput output{
      out_params->dataPtr,
      out_params->pitch,
      static_cast<int>(output_rect.width()),
      static_cast<int>(output_rect.height()),
      format};
  return RenderProgram(
      input,
      output,
      {src_rect, anchor_point, crop_box, angle},
      shadow_lift_percent,
      lift_shadow_black_point,
      exposure,
      stream);
}
} // namespace hm::playcropper
