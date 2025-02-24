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

void print_caps(const GstCaps* caps) {
  gchar* caps_str = gst_caps_to_string(caps);
  g_print("Caps: %s\n", caps_str);
  g_free(caps_str);
}

void print_caps_details(const GstCaps* caps) {
  int size = gst_caps_get_size(caps);
  for (int i = 0; i < size; i++) {
    const GstStructure* structure = gst_caps_get_structure(caps, i);
    gchar* structure_str = gst_structure_to_string(structure);
    g_print("Structure %d: %s\n", i, structure_str);
    g_free(structure_str);
  }
}

gint get_batch_size_from_caps(GstCaps* caps) {
  gint batch_size = 1; // Default to 1 if not specified
  gboolean is_batched = FALSE;

  if (!caps || gst_caps_is_empty(caps)) {
    GST_WARNING("Caps is NULL or empty");
    return batch_size;
  }

  // Get the first structure (typically where batch info would be)
  GstStructure* structure = gst_caps_get_structure(caps, 0);
  if (!structure) {
    GST_WARNING("No structure in caps");
    return batch_size;
  }

  // First check if the stream is batched
  if (gst_structure_get_boolean(structure, "batched", &is_batched)) {
    if (is_batched) {
      // If batched, try to get the batch-size field
      if (gst_structure_get_int(structure, "batch-size", &batch_size)) {
        GST_DEBUG("Found batch-size: %d", batch_size);
      } else {
        GST_WARNING("Stream is batched but no batch-size specified");
      }
    } else {
      GST_DEBUG("Stream is not batched");
    }
  } else {
    GST_DEBUG("No batched field in caps, assuming batch size of 1");
  }

  return batch_size;
}

} // namespace gst
} // namespace hm
