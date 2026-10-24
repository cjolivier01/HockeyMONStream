#include "hstream/src/libs/program/VaProgramOutput.h"

namespace hm::playcropper {
absl::StatusOr<YuvOutput> DescribeVaProgramOutput(const gpu::VaHipFrame& frame, const GstVideoInfo& info) {
  const bool ten_bit = frame.format() == gpu::VaHipFrame::Format::kP010;
  if ((!ten_bit && frame.format() != gpu::VaHipFrame::Format::kNv12) ||
      frame.width() != static_cast<unsigned>(info.width) || frame.height() != static_cast<unsigned>(info.height) ||
      !info.finfo || GST_VIDEO_INFO_FORMAT(&info) != (ten_bit ? GST_VIDEO_FORMAT_P010_10LE : GST_VIDEO_FORMAT_NV12))
    return absl::InvalidArgumentError("VA Program output requires matching NV12/P010 caps and surface geometry");
  if (info.interlace_mode != GST_VIDEO_INTERLACE_MODE_PROGRESSIVE || info.chroma_site != GST_VIDEO_CHROMA_SITE_JPEG)
    return absl::InvalidArgumentError("VA Program output requires progressive frames with centered 2x2 chroma");
  if (info.colorimetry.transfer == GST_VIDEO_TRANSFER_UNKNOWN ||
      info.colorimetry.transfer == GST_VIDEO_TRANSFER_SMPTE2084 ||
      info.colorimetry.transfer == GST_VIDEO_TRANSFER_ARIB_STD_B67)
    return absl::InvalidArgumentError("VA Program output requires explicit SDR color transfer");
  double kr, kb;
  if (!gst_video_color_matrix_get_Kr_Kb(info.colorimetry.matrix, &kr, &kb))
    return absl::InvalidArgumentError("VA Program output requires a negotiated color matrix");
  YuvEncoding color{};
  color.kr = kr;
  color.kb = kb;
  const float factor = ten_bit ? 4 : 1;
  if (info.colorimetry.range == GST_VIDEO_COLOR_RANGE_16_235) {
    color.y_offset = 16 * factor;
    color.y_range = 219 * factor;
    color.uv_offset = 128 * factor;
    color.uv_range = 224 * factor;
  } else if (info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255) {
    color.y_offset = 0;
    color.y_range = ten_bit ? 1023 : 255;
    color.uv_offset = 128 * factor;
    color.uv_range = color.y_range;
  } else
    return absl::InvalidArgumentError("VA Program output requires a negotiated color range");
  const auto& y = frame.plane(0);
  const auto& uv = frame.plane(1);
  return YuvOutput{
      y.data,
      uv.data,
      y.pitch,
      uv.pitch,
      info.width,
      info.height,
      ten_bit ? gpu::PixelFormat::kP010 : gpu::PixelFormat::kNv12,
      color};
}
} // namespace hm::playcropper
