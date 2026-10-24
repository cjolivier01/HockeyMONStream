#include "hstream/src/libs/gpu/vaapi/VaOutputPool.h"

#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvadisplay_drm.h>
#include <gst/va/gstvapool.h>
#include <gst/va/gstvautils.h>
#include <gst/video/video-info-dma.h>
#include <va/va.h>
#include "hstream/src/libs/gpu/Runtime.h"

#include <filesystem>
#include <string>

namespace hm::gpu {
absl::StatusOr<std::unique_ptr<VaOutputPool>> VaOutputPool::Create(
    int device,
    const GstVideoInfo& info,
    unsigned maximum_buffers) {
  if (!info.finfo)
    return absl::InvalidArgumentError("VA output requires a pixel format");
  const auto format = GST_VIDEO_INFO_FORMAT(&info);
  const bool yuv = format == GST_VIDEO_FORMAT_NV12 || format == GST_VIDEO_FORMAT_P010_10LE;
  double kr, kb;
  const bool color_valid = yuv
      ? gst_video_color_matrix_get_Kr_Kb(info.colorimetry.matrix, &kr, &kb) &&
          (info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255 ||
           info.colorimetry.range == GST_VIDEO_COLOR_RANGE_16_235)
      : info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255 && info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_RGB;
  if (!gst_is_initialized() || !maximum_buffers || device < 0 || (!yuv && format != GST_VIDEO_FORMAT_RGBA) ||
      (yuv && ((info.width | info.height) & 1)) || info.width <= 0 || info.height <= 0 || info.width >= (1 << 24) ||
      info.height >= (1 << 24) || info.fps_n <= 0 || info.fps_d <= 0 || info.par_n <= 0 || info.par_d <= 0 ||
      info.interlace_mode != GST_VIDEO_INTERLACE_MODE_PROGRESSIVE || info.size == 0 || info.size > G_MAXUINT ||
      !color_valid || info.colorimetry.transfer == GST_VIDEO_TRANSFER_UNKNOWN ||
      info.colorimetry.transfer == GST_VIDEO_TRANSFER_SMPTE2084 ||
      info.colorimetry.transfer == GST_VIDEO_TRANSFER_ARIB_STD_B67 ||
      info.colorimetry.primaries == GST_VIDEO_COLOR_PRIMARIES_UNKNOWN)
    return absl::InvalidArgumentError(
        "VA output requires explicit progressive SDR geometry/colorimetry and a buffer bound");
  char pci[32]{};
  const auto status = hipDeviceGetPCIBusId(pci, sizeof(pci), device);
  if (status != hipSuccess)
    return absl::InvalidArgumentError(std::string("HIP output device: ") + hipGetErrorString(status));
  auto result = std::unique_ptr<VaOutputPool>(new VaOutputPool());
  const auto path = std::string("/dev/dri/by-path/pci-") + pci + "-render";
  // VA element factories compare their render path to the context verbatim.
  // Resolve the PCI alias so they recognize the display as their own device.
  std::error_code error;
  const auto render = std::filesystem::canonical(path, error);
  if (error)
    return absl::UnavailableError("Cannot resolve HIP render node " + path + ": " + error.message());
  result->display_ = gst_va_display_drm_new_from_path(render.c_str());
  if (!result->display_)
    return absl::UnavailableError("Cannot open VA on HIP device " + path);
  GstVideoInfoDmaDrm dma{};
  if (!gst_video_info_dma_drm_from_video_info(&dma, &info, 0))
    return absl::InvalidArgumentError("Cannot describe linear DMA-BUF output");
  result->caps_ = gst_video_info_dma_drm_to_caps(&dma);
  if (!result->caps_)
    return absl::InvalidArgumentError("Cannot create VA output caps");
  auto* allocator = gst_va_dmabuf_allocator_new(result->display_);
  if (!allocator)
    return absl::UnavailableError("Cannot create VA DMA-BUF allocator");
  result->pool_ = gst_va_pool_new_with_config(
      result->caps_,
      0,
      maximum_buffers,
      VA_SURFACE_ATTRIB_USAGE_HINT_GENERIC,
      GST_VA_FEATURE_DISABLED,
      allocator,
      nullptr);
  gst_object_unref(allocator);
  if (!result->pool_ || !gst_buffer_pool_set_active(result->pool_, TRUE))
    return absl::UnavailableError("Cannot activate VA output pool");
  return result;
}
absl::StatusOr<GstBuffer*> VaOutputPool::Acquire() {
  GstBufferPoolAcquireParams params{};
  params.flags = GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT;
  GstBuffer* buffer{};
  const auto flow = gst_buffer_pool_acquire_buffer(pool_, &buffer, &params);
  if (flow == GST_FLOW_EOS)
    return absl::ResourceExhaustedError("VA output pool is fully borrowed");
  if (flow != GST_FLOW_OK)
    return absl::UnavailableError(std::string("VA output acquisition: ") + gst_flow_get_name(flow));
  return buffer;
}
void VaOutputPool::SetDisplayContext(GstElement* element) const {
  auto* context = gst_context_new(GST_VA_DISPLAY_HANDLE_CONTEXT_TYPE_STR, TRUE);
  gst_context_set_va_display(context, display_);
  gst_element_set_context(element, context);
  gst_context_unref(context);
}
VaOutputPool::~VaOutputPool() {
  if (pool_) {
    // GstBufferPool keeps outstanding buffers valid and releases their storage
    // when the last consumer returns them; deactivation prevents new borrows.
    gst_buffer_pool_set_active(pool_, FALSE);
    gst_object_unref(pool_);
  }
  if (caps_)
    gst_caps_unref(caps_);
  if (display_)
    gst_object_unref(display_);
}
} // namespace hm::gpu
