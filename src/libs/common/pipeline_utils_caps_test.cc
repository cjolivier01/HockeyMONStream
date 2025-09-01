// Integration-like test for pushing RECONFIGURE + CAPS on a pad.

#include "hstream/src/libs/common/pipeline_utils.h"

#include <cassert>
#include <gst/gst.h>

struct ProbeState {
  bool saw_reconfigure{false};
  bool saw_caps{false};
  int width{0};
  int height{0};
};

static GstPadProbeReturn event_probe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
  ProbeState* st = reinterpret_cast<ProbeState*>(user_data);
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    GstEvent* ev = gst_pad_probe_info_get_event(info);
    switch (GST_EVENT_TYPE(ev)) {
      case GST_EVENT_RECONFIGURE:
        st->saw_reconfigure = true;
        break;
      case GST_EVENT_CAPS: {
        GstCaps* caps = nullptr;
        gst_event_parse_caps(ev, &caps);
        if (caps && gst_caps_get_size(caps) > 0) {
          GstStructure* s = gst_caps_get_structure(caps, 0);
          gst_structure_get_int(s, "width", &st->width);
          gst_structure_get_int(s, "height", &st->height);
          st->saw_caps = true;
        }
        break;
      }
      default:
        break;
    }
  }
  return GST_PAD_PROBE_OK;
}

int main() {
  gst_init(nullptr, nullptr);

  GstElement* src = gst_element_factory_make("fakesrc", "src");
  assert(src);
  GstPad* srcpad = gst_element_get_static_pad(src, "src");
  assert(srcpad);

  ProbeState st;
  gst_pad_add_probe(srcpad, (GstPadProbeType)(GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM), event_probe, &st, nullptr);

  // Call helper
  const int W = 1280, H = 720;
  bool ok = hm::push_reconfigure_and_caps_on_pad(srcpad, W, H, "RGBA", true);
  assert(ok);

  // Ensure probe saw expected events
  assert(st.saw_reconfigure);
  assert(st.saw_caps);
  assert(st.width == W);
  assert(st.height == H);

  gst_object_unref(srcpad);
  gst_object_unref(src);
  return 0;
}

