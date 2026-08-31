#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/FatalOutputError.h"

#include "absl/status/status.h"

#include <gst/gst.h>

#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  bool ok = true;

  GstElement* pipeline = gst_pipeline_new("fatal-output-error-test");
  GstElement* element = gst_element_factory_make("identity", "videoprep-under-test");
  if (!pipeline || !element) {
    if (pipeline) {
      gst_object_unref(pipeline);
    }
    if (element) {
      gst_object_unref(element);
    }
    std::cerr << "FAIL: required GStreamer elements are unavailable\n";
    return 1;
  }
  gst_bin_add(GST_BIN(pipeline), element);

  GstBus* bus = gst_element_get_bus(pipeline);
  const absl::Status failure = absl::ResourceExhaustedError("Could not allocate the FP16 stitched canvas");
  ok &= expect(
      hm::videoprep::post_fatal_output_error(element, failure),
      "a resource-exhaustion output failure must be posted to the pipeline bus");

  GstMessage* message = gst_bus_timed_pop_filtered(bus, 0, GST_MESSAGE_ERROR);
  ok &= expect(message != nullptr, "the pipeline bus must receive a fatal error message");
  if (message) {
    GError* error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    const std::string error_message = error && error->message ? error->message : "";
    ok &= expect(error && error->domain == GST_LIBRARY_ERROR, "the message must use the GStreamer library domain");
    ok &= expect(error && error->code == GST_LIBRARY_ERROR_FAILED, "the message must be classified as fatal");
    ok &= expect(
        error_message.find("RESOURCE_EXHAUSTED") != std::string::npos &&
            error_message.find("Could not allocate the FP16 stitched canvas") != std::string::npos,
        "the fatal bus message must preserve the original status and diagnostic");
    g_clear_error(&error);
    g_free(debug);
    gst_message_unref(message);
  }

  ok &= expect(
      !hm::videoprep::post_fatal_output_error(element, absl::CancelledError("shutdown")),
      "cooperative cancellation must not be posted as a fatal error");
  GstMessage* cancellation_message = gst_bus_timed_pop_filtered(bus, 0, GST_MESSAGE_ERROR);
  ok &= expect(cancellation_message == nullptr, "cooperative cancellation must leave the pipeline bus clear");
  if (cancellation_message) {
    gst_message_unref(cancellation_message);
  }

  gst_object_unref(bus);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  return ok ? 0 : 1;
}
