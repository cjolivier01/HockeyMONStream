#include "gst_utils.h"
#include "nvbufsurface.h"

namespace hm {
namespace gst {
void inspect_nvbufsurface_dtype(GstBuffer* buffer) {
  // Map the GstBuffer to retrieve the NvBufSurface
  GstMapInfo map_info;
  if (!gst_buffer_map(buffer, &map_info, GST_MAP_READ)) {
    GST_ERROR("Failed to map GstBuffer.");
    return;
  }

  NvBufSurface* nvbuf_surface = (NvBufSurface*)map_info.data;
  if (!nvbuf_surface) {
    GST_ERROR("NvBufSurface is null.");
    gst_buffer_unmap(buffer, &map_info);
    return;
  }

  // Iterate through surfaces in the NvBufSurface
  for (uint32_t i = 0; i < nvbuf_surface->numFilled; i++) {
    NvBufSurfaceParams& surface_params = nvbuf_surface->surfaceList[i];
    GST_INFO(
        "Surface %d: width=%d, height=%d, pitch=%d",
        i,
        surface_params.width,
        surface_params.height,
        surface_params.pitch);

    switch (surface_params.colorFormat) {
      case NVBUF_COLOR_FORMAT_RGBA:
        GST_INFO("Surface %d has color format: NVBUF_COLOR_FORMAT_RGBA (dtype: uint8_t).", i);
        break;

      case NVBUF_COLOR_FORMAT_NV12:
        GST_INFO("Surface %d has color format: NVBUF_COLOR_FORMAT_NV12 (dtype: uint8_t, YUV 4:2:0).", i);
        break;

      case NVBUF_COLOR_FORMAT_UYVY:
        GST_INFO("Surface %d has color format: NVBUF_COLOR_FORMAT_UYVY (dtype: uint8_t, YUV 4:2:2).", i);
        break;

      default:
        GST_WARNING("Surface %d has an unsupported or unknown color format: %d", i, surface_params.colorFormat);
        break;
    }
  }

  // Unmap the buffer
  gst_buffer_unmap(buffer, &map_info);
}
} // namespace gst
} // namespace hm
