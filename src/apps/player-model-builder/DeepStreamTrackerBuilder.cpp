#include "DeepStreamTrackerBuilder.h"

#include <gst/gst.h>

#include <memory>
#include <stdexcept>

void BuildDeepStreamTracker(const std::string& config, const std::string& library, int gpu_id) {
  gst_init(nullptr, nullptr);
  auto release = [](GstElement* element) {
    gst_element_set_state(element, GST_STATE_NULL);
    gst_object_unref(element);
  };
  std::unique_ptr<GstElement, decltype(release)> tracker(gst_element_factory_make("nvtracker", nullptr), release);
  if (!tracker)
    throw std::runtime_error("DeepStream's native nvtracker plugin is unavailable");
  g_object_set(
      tracker.get(),
      "ll-config-file",
      config.c_str(),
      "ll-lib-file",
      library.c_str(),
      "gpu-id",
      static_cast<guint>(gpu_id),
      "tracker-width",
      640U,
      "tracker-height",
      384U,
      nullptr);
  // GstBaseTransform::start initializes NvDCF and its appearance engine. No
  // media source, video buffers, metadata, or pixel readback is required here.
  if (gst_element_set_state(tracker.get(), GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
    throw std::runtime_error("DeepStream could not prepare its default ReID model; see native tracker diagnostics");
  GstState current = GST_STATE_NULL;
  if (gst_element_get_state(tracker.get(), &current, nullptr, GST_CLOCK_TIME_NONE) == GST_STATE_CHANGE_FAILURE ||
      current != GST_STATE_PAUSED)
    throw std::runtime_error("DeepStream ReID preparation did not reach the initialized state");
}
