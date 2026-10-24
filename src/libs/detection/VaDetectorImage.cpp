#include "hstream/src/libs/detection/VaDetectorImage.h"

namespace hm::detection {
absl::StatusOr<Image> DescribeVaDetectorImage(const gpu::VaHipFrame& frame, const GstVideoInfo& info) {
  if (frame.width() != static_cast<unsigned>(info.width) || frame.height() != static_cast<unsigned>(info.height))
    return absl::InvalidArgumentError("VA detector caps and imported geometry differ");
  if (info.colorimetry.transfer == GST_VIDEO_TRANSFER_SMPTE2084 ||
      info.colorimetry.transfer == GST_VIDEO_TRANSFER_ARIB_STD_B67)
    return absl::UnimplementedError("Tone-map HDR before SDR detector preprocessing");
  const auto& y = frame.plane(0);
  Image image{y.data, nullptr, y.pitch, 0, info.width, info.height, PixelFormat::kRgba8, 0, 0, 0, 0, 0, 0, 0};
  if (frame.format() == gpu::VaHipFrame::Format::kRgba8) {
    if (GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_RGBA)
      return absl::InvalidArgumentError("VA detector caps and imported pixel format differ");
    return image;
  }
  if (frame.format() != gpu::VaHipFrame::Format::kNv12 && frame.format() != gpu::VaHipFrame::Format::kP010)
    return absl::UnimplementedError("Unsupported VA detector pixel format");
  const bool ten_bit = frame.format() == gpu::VaHipFrame::Format::kP010;
  if (GST_VIDEO_INFO_FORMAT(&info) != (ten_bit ? GST_VIDEO_FORMAT_P010_10LE : GST_VIDEO_FORMAT_NV12))
    return absl::InvalidArgumentError("VA detector caps and imported pixel format differ");
  image.format = ten_bit ? PixelFormat::kP010 : PixelFormat::kNv12;
  const auto& uv = frame.plane(1);
  image.plane1 = uv.data;
  image.pitch1 = uv.pitch;
  double kr, kb;
  if (!gst_video_color_matrix_get_Kr_Kb(info.colorimetry.matrix, &kr, &kb))
    return absl::InvalidArgumentError("VA detector requires negotiated YUV color matrix");
  const double kg = 1 - kr - kb;
  const float codes = ten_bit ? 1023.f : 255.f;
  const float factor = ten_bit ? 4.f : 1.f;
  float chroma_scale;
  if (info.colorimetry.range == GST_VIDEO_COLOR_RANGE_16_235) {
    image.y_offset = 16 * factor * 255 / codes;
    image.uv_offset = 128 * factor * 255 / codes;
    image.y_scale = codes / (219 * factor);
    chroma_scale = codes / (224 * factor);
  } else if (info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255) {
    image.y_offset = 0;
    image.uv_offset = 128 * factor * 255 / codes;
    image.y_scale = 1;
    chroma_scale = 1;
  } else
    return absl::InvalidArgumentError("VA detector requires negotiated YUV range");
  image.r_v = 2 * (1 - kr) * chroma_scale;
  image.b_u = 2 * (1 - kb) * chroma_scale;
  image.g_u = -2 * kb * (1 - kb) / kg * chroma_scale;
  image.g_v = -2 * kr * (1 - kr) / kg * chroma_scale;
  return image;
}
} // namespace hm::detection
