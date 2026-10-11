#include "AsyncInferenceSeekRecovery.h"

#include <gst/base/gstbasetransform.h>

namespace hm {
namespace {

GQuark recovery_probe_key() {
  return g_quark_from_static_string("hstream-async-inference-seek-recovery");
}

GstPadProbeReturn prime_after_seek(GstPad* pad, GstPadProbeInfo* info, gpointer) {
  g_object_set_qdata(G_OBJECT(pad), recovery_probe_key(), nullptr);
  gst_pad_remove_probe(pad, GST_PAD_PROBE_INFO_ID(info));
  GstElement* element = gst_pad_get_parent_element(pad);
  auto* transform = GST_BASE_TRANSFORM(element);
  GstPad* src = GST_BASE_TRANSFORM_SRC_PAD(transform);
  GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  GST_PAD_PROBE_INFO_DATA(info) = nullptr;
  // The normal chain takes ownership. HANDLED below prevents a second call or
  // unref; this probe removes itself, so steady-state input has no extra work.
  GstFlowReturn flow = GST_PAD_CHAINFUNC(pad)(pad, GST_OBJECT(element), buffer);
  if (flow == GST_FLOW_FLUSHING && !GST_PAD_IS_FLUSHING(pad) && !GST_PAD_IS_FLUSHING(src)) {
    // nvinfer can retain the old worker's FLUSHING result after FLUSH_STOP.
    // Its first new input is already queued when generate_output returns that
    // stale result. Let the worker finish it before allowing the upstream queue
    // to pause permanently. A serialized marker drains the worker through the
    // plugin's public event handler; no SDK-private layout or model reload.
    GstEvent* barrier =
        gst_event_new_custom(GST_EVENT_CUSTOM_DOWNSTREAM, gst_structure_new_empty("hstream-inference-seek-barrier"));
    if (gst_pad_send_event(pad, barrier) && !GST_PAD_IS_FLUSHING(pad) && !GST_PAD_IS_FLUSHING(src)) {
      GstBuffer* output = nullptr;
      flow = GST_BASE_TRANSFORM_GET_CLASS(transform)->generate_output(transform, &output);
      if (output) {
        // The supported asynchronous contract returns no buffer, but preserve
        // ownership and downstream errors if an implementation does supply one.
        if (flow == GST_FLOW_OK)
          flow = gst_pad_push(src, output);
        else
          gst_buffer_unref(output);
      }
      g_print(
          "HSTREAM_SEEK_INFERENCE element=%s first-buffer-result=%s\n",
          GST_ELEMENT_NAME(element),
          gst_flow_get_name(flow));
    }
  }
  GST_PAD_PROBE_INFO_FLOW_RETURN(info) = flow;
  gst_object_unref(element);
  return GST_PAD_PROBE_HANDLED;
}

} // namespace

bool ArmAsyncInferenceSeekRecovery(GstElement* element) {
  if (!GST_IS_BASE_TRANSFORM(element) || !GST_BASE_TRANSFORM_GET_CLASS(element)->generate_output)
    return false;
  GstPad* sink = GST_BASE_TRANSFORM_SINK_PAD(element);
  if (!sink || !GST_PAD_CHAINFUNC(sink))
    return false;
  const gulong previous = GPOINTER_TO_SIZE(g_object_get_qdata(G_OBJECT(sink), recovery_probe_key()));
  if (previous)
    gst_pad_remove_probe(sink, previous);
  const gulong id = gst_pad_add_probe(sink, GST_PAD_PROBE_TYPE_BUFFER, prime_after_seek, nullptr, nullptr);
  g_object_set_qdata(G_OBJECT(sink), recovery_probe_key(), GSIZE_TO_POINTER(id));
  return id != 0;
}

} // namespace hm
