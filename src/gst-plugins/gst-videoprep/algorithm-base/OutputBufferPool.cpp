#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/OutputBufferPool.h"

#include <sstream>

namespace hm::videoprep {

GstBufferPool* configure_output_pool(GstBufferPool* pool, GstStructure* config) {
  if (!gst_buffer_pool_set_config(pool, config) || !gst_buffer_pool_set_active(pool, TRUE)) {
    gst_object_unref(pool);
    return nullptr;
  }
  return pool;
}

absl::Status output_buffer_failure_status(
    GstBufferPool* pool,
    GstFlowReturn flow,
    unsigned gpu,
    const std::optional<OutputGpuMemory>& memory,
    const std::string& memory_error) {
  std::ostringstream message;
  message << "Could not acquire an output buffer (GPU): " << gst_flow_get_name(flow) << " (" << flow << ")";
  GstStructure* config = pool ? gst_buffer_pool_get_config(pool) : nullptr;
  if (config) {
    GstCaps* caps = nullptr;
    guint minimum = 0, maximum = 0;
    if (gst_buffer_pool_config_get_params(config, &caps, nullptr, &minimum, &maximum)) {
      if (caps && !gst_caps_is_empty(caps) && !gst_caps_is_any(caps)) {
        const GstStructure* shape = gst_caps_get_structure(caps, 0);
        gint width = 0, height = 0;
        const gchar* format = gst_structure_get_string(shape, "format");
        if (gst_structure_get_int(shape, "width", &width) && gst_structure_get_int(shape, "height", &height))
          message << "; canvas " << width << "x" << height;
        if (format)
          message << " " << format;
      }
      message << "; pool minimum " << minimum << ", maximum ";
      if (maximum)
        message << maximum;
      else
        message << "unlimited";
      message << " buffers";
    }
    guint batch_size = 0;
    if (gst_structure_get_uint(config, "batch-size", &batch_size))
      message << "; batch size " << batch_size;
    gst_structure_free(config);
  }
  message << "; GPU " << gpu << " memory ";
  if (memory) {
    constexpr uint64_t kMiB = 1024ULL * 1024ULL;
    message << memory->free_bytes / kMiB << " MiB free of " << memory->total_bytes / kMiB << " MiB";
  } else {
    message << "unavailable";
    if (!memory_error.empty())
      message << " (" << memory_error << ")";
  }
  return absl::InternalError(message.str());
}

} // namespace hm::videoprep
