#include <gst/gst.h>

extern "C" GType gst_hstream_lossless_mux_get_type(void);

bool register_hstream_lossless_nvstreammux() {
  static gsize registration_result = 0;
  if (g_once_init_enter(&registration_result)) {
    const gboolean registered =
        gst_element_register(nullptr, "hstreamlosslessmux", GST_RANK_NONE, gst_hstream_lossless_mux_get_type());
    g_once_init_leave(&registration_result, registered ? 1 : 2);
  }
  return registration_result == 1;
}

bool is_hstream_lossless_nvstreammux(GstElement* element) {
  if (!element) {
    return false;
  }
  GstElementFactory* factory = gst_element_get_factory(element);
  if (!factory) {
    return false;
  }
  return g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)), "hstreamlosslessmux") == 0;
}
