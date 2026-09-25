// Rejected CAPS during shutdown must not strand the mux context lock. This
// exercises only GStreamer events: no input buffers or GPU allocation needed.
#include <gst/gst.h>

#include <unistd.h>
#include <csignal>
#include <cstring>
#include <iostream>

#include "hstream/src/apps/apps-common/HStreamLosslessMux.h"

namespace {
gboolean AcceptEvent(GstPad*, GstObject*, GstEvent* event) {
  gst_event_unref(event);
  return TRUE;
}

void Timeout(int) {
  constexpr char message[] = "FAIL: rejected caps stranded the mux state transition\n";
  const auto written = write(STDERR_FILENO, message, sizeof(message) - 1);
  (void)written;
  _exit(1);
}

bool RejectedCaps(const char* name, const char* caps_text, bool preexisting_caps) {
  alarm(10);
  GstElement* mux = gst_element_factory_make("hstreamlosslessmux", nullptr);
  if (!mux)
    return false;
  g_object_set(mux, "batch-size", 1, "num-surfaces-per-frame", 1, nullptr);
  GstPad* input = gst_element_request_pad_simple(mux, "sink_0");
  GstPad* source = gst_element_get_static_pad(mux, "src");
  GstPad* downstream = gst_pad_new("downstream", GST_PAD_SINK);
  gst_pad_set_event_function(downstream, AcceptEvent);
  gst_pad_set_active(downstream, TRUE);
  if (!input || !source || gst_pad_link(source, downstream) != GST_PAD_LINK_OK ||
      gst_element_set_state(mux, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
    return false;
  gst_pad_send_event(input, gst_event_new_stream_start("rejected-caps-test"));
  GstCaps* caps = gst_caps_from_string(caps_text);
  if (preexisting_caps) {
    gst_pad_send_event(input, gst_event_new_caps(caps));
    if (!gst_pad_has_current_caps(source))
      return false;
  }
  // Downstream can be flushing while upstream still delivers CAPS. Send the
  // flush on this source pad only so the sink CAPS handler still executes.
  gst_pad_push_event(source, gst_event_new_flush_start());
  if (!GST_PAD_IS_FLUSHING(source) || GST_PAD_IS_FLUSHING(input) ||
      static_cast<bool>(gst_pad_has_current_caps(source)) != preexisting_caps)
    return false;
  // GstPad's sticky-event handling can return TRUE here even though the mux's
  // downstream push failed; completion of the state transition is the oracle.
  gst_pad_send_event(input, gst_event_new_caps(caps));
  gst_caps_unref(caps);
  std::cerr << "Stopping mux after rejected " << name << '\n';
  const bool stopped = gst_element_set_state(mux, GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE;
  gst_element_release_request_pad(mux, input);
  gst_object_unref(input);
  gst_pad_unlink(source, downstream);
  gst_object_unref(source);
  gst_pad_set_active(downstream, FALSE);
  gst_object_unref(downstream);
  gst_object_unref(mux);
  alarm(0);
  if (stopped)
    std::cout << "PASS rejected " << name << " permits mux teardown\n";
  return stopped;
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  std::signal(SIGALRM, Timeout);
  if (!register_hstream_lossless_nvstreammux())
    return 1;
  struct Case {
    const char* name;
    const char* caps;
    bool preexisting_caps;
  } cases[] = {
      {"video-update", "video/x-raw(memory:NVMM),format=RGBA,width=320,height=240,framerate=30/1", false},
      {"audio-caps", "audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=1", false},
      {"audio-update", "audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=1", true},
  };
  bool selected = false;
  for (const auto& test : cases) {
    if (argc > 1 && std::strcmp(argv[1], test.name) != 0)
      continue;
    selected = true;
    if (!RejectedCaps(test.name, test.caps, test.preexisting_caps))
      return 1;
  }
  return selected ? 0 : 1;
}
