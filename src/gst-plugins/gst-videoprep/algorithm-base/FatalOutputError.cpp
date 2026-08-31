#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/FatalOutputError.h"

#include <string>

namespace hm::videoprep {

bool post_fatal_output_error(GstElement* element, const absl::Status& status) {
  if (!element || status.ok() || absl::IsCancelled(status)) {
    return false;
  }

  const std::string message = "Video preparation failed: " + status.ToString();
  GError* error = g_error_new_literal(GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, message.c_str());
  return gst_element_post_message(element, gst_message_new_error(GST_OBJECT(element), error, nullptr));
}

} // namespace hm::videoprep
