#include "hstream/src/apps/apps-common/PipelineStartupError.h"

#include <string>

namespace hm::gst {

absl::Status PipelineStartupFailure(GstElement* pipeline, const char* fallback) {
  std::string description(fallback);
  GstBus* bus = pipeline ? gst_element_get_bus(pipeline) : nullptr;
  if (!bus)
    return absl::InternalError(description);
  GstMessage* message = gst_bus_timed_pop_filtered(bus, 0, GST_MESSAGE_ERROR);
  gst_object_unref(bus);
  if (!message)
    return absl::InternalError(description);

  GError* error = nullptr;
  gst_message_parse_error(message, &error, nullptr);
  if (error && error->message && *error->message) {
    description += ": ";
    const char* source = GST_MESSAGE_SRC(message) ? GST_OBJECT_NAME(GST_MESSAGE_SRC(message)) : nullptr;
    if (source && *source) {
      description += source;
      description += ": ";
    }
    description += error->message;
  }
  g_clear_error(&error);
  gst_message_unref(message);
  return absl::InternalError(description);
}

} // namespace hm::gst
