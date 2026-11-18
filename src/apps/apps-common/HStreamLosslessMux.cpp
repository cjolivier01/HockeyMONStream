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

namespace {
// Written once per pipeline construction by create_multi_source_bin(), on the
// thread that builds the graph, and read later on the same thread while that
// graph is assembled.
bool g_lossless_nvstreammux_selected = false;
} // namespace

bool hstream_lossless_nvstreammux_selected() {
  return g_lossless_nvstreammux_selected;
}

void set_hstream_lossless_nvstreammux_selected(bool selected) {
  g_lossless_nvstreammux_selected = selected;
}
