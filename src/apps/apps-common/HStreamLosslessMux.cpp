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
