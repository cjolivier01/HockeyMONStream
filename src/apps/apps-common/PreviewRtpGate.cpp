#include "PreviewRtpGate.h"

#include <mutex>
#include <new>
#include <utility>

namespace {
struct State {
  std::mutex flow, state;
  std::shared_ptr<hm::preview::TransportMonitor> monitor;
  std::uint64_t peer{0};
  bool transitioning{false}, ready{false};
};
typedef struct _GstHmPreviewRtpGate {
  GstElement parent;
  GstPad *sink, *src;
  State* state;
} GstHmPreviewRtpGate;
typedef struct _GstHmPreviewRtpGateClass {
  GstElementClass parent_class;
} GstHmPreviewRtpGateClass;
G_DEFINE_TYPE(GstHmPreviewRtpGate, gst_hm_preview_rtp_gate, GST_TYPE_ELEMENT)

GstStaticPadTemplate input_template =
    GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS("application/x-rtp"));
GstStaticPadTemplate output_template =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("application/x-rtp"));

GstFlowReturn forward(GstObject* parent, GstBuffer* buffer, GstBufferList* list) noexcept {
  auto* self = reinterpret_cast<GstHmPreviewRtpGate*>(parent);
  auto& state = *self->state;
  try {
    std::lock_guard<std::mutex> flow(state.flow);
    bool ready;
    {
      std::lock_guard<std::mutex> lock(state.state);
      ready = state.ready && !state.transitioning;
    }
    if (ready) {
      if (list) {
        list = gst_buffer_list_make_writable(list);
        if (state.monitor->FilterRtp(list)) {
          auto* output = std::exchange(list, nullptr);
          return gst_pad_push_list(self->src, output);
        }
      } else if (state.monitor->Rtp(buffer)) {
        auto* output = std::exchange(buffer, nullptr);
        return gst_pad_push(self->src, output);
      }
    }
  } catch (...) {
    state.monitor->ProbeFailure();
  }
  if (buffer)
    gst_buffer_unref(buffer);
  if (list)
    gst_buffer_list_unref(list);
  return GST_FLOW_OK;
}

GstFlowReturn chain(GstPad*, GstObject* parent, GstBuffer* buffer) {
  return forward(parent, buffer, nullptr);
}
GstFlowReturn chain_list(GstPad*, GstObject* parent, GstBufferList* list) {
  return forward(parent, nullptr, list);
}
gboolean event(GstPad*, GstObject* parent, GstEvent* event) {
  auto* self = reinterpret_cast<GstHmPreviewRtpGate*>(parent);
  // Flush must be able to release a downstream wait that owns the flow lock.
  if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START)
    return gst_pad_push_event(self->src, event);
  std::lock_guard<std::mutex> flow(self->state->flow);
  return gst_pad_push_event(self->src, event);
}
void finalize(GObject* object) {
  delete reinterpret_cast<GstHmPreviewRtpGate*>(object)->state;
  G_OBJECT_CLASS(gst_hm_preview_rtp_gate_parent_class)->finalize(object);
}
void gst_hm_preview_rtp_gate_class_init(GstHmPreviewRtpGateClass* type) {
  G_OBJECT_CLASS(type)->finalize = finalize;
  auto* element = GST_ELEMENT_CLASS(type);
  gst_element_class_set_static_metadata(element, "Preview RTP barrier", "Generic", "Fences peer output", "HStream");
  gst_element_class_add_static_pad_template(element, &input_template);
  gst_element_class_add_static_pad_template(element, &output_template);
}
void gst_hm_preview_rtp_gate_init(GstHmPreviewRtpGate* self) {
  self->state = new (std::nothrow) State;
  self->sink = gst_pad_new_from_static_template(&input_template, "sink");
  self->src = gst_pad_new_from_static_template(&output_template, "src");
  gst_pad_set_chain_function(self->sink, chain);
  gst_pad_set_chain_list_function(self->sink, chain_list);
  gst_pad_set_event_function(self->sink, event);
  gst_element_add_pad(GST_ELEMENT(self), self->sink);
  gst_element_add_pad(GST_ELEMENT(self), self->src);
}
GstHmPreviewRtpGate* get(GstElement* element) {
  return element && G_TYPE_CHECK_INSTANCE_TYPE(element, gst_hm_preview_rtp_gate_get_type())
      ? reinterpret_cast<GstHmPreviewRtpGate*>(element)
      : nullptr;
}
} // namespace

namespace hm::preview {
GstElement* rtp_gate_create(const char* name, std::shared_ptr<TransportMonitor> monitor) {
  if (!monitor)
    return nullptr;
  auto* self =
      reinterpret_cast<GstHmPreviewRtpGate*>(g_object_new(gst_hm_preview_rtp_gate_get_type(), "name", name, nullptr));
  if (!self || !self->state) {
    if (self)
      gst_object_unref(self);
    return nullptr;
  }
  self->state->monitor = std::move(monitor);
  return GST_ELEMENT(self);
}
bool rtp_gate_begin_transition(GstElement* element, std::uint64_t peer) {
  auto* self = get(element);
  if (!self || !peer)
    return false;
  std::lock_guard<std::mutex> lock(self->state->state);
  if (peer <= self->state->peer)
    return false;
  self->state->peer = peer;
  self->state->transitioning = true;
  self->state->ready = false;
  return true;
}
bool rtp_gate_finish_transition(GstElement* element, std::uint64_t peer, const std::function<bool()>& mutation) {
  auto* self = get(element);
  if (!self || !mutation)
    return false;
  auto& state = *self->state;
  std::lock_guard<std::mutex> flow(state.flow);
  {
    std::lock_guard<std::mutex> lock(state.state);
    if (state.peer != peer || !state.transitioning)
      return false;
  }
  if (!state.monitor->SetPeer(peer) || !mutation())
    return false;
  std::lock_guard<std::mutex> lock(state.state);
  if (state.peer != peer)
    return false;
  state.transitioning = false;
  return true;
}
bool rtp_gate_set_ready(GstElement* element, std::uint64_t peer, bool ready) {
  auto* self = get(element);
  if (!self)
    return false;
  auto& state = *self->state;
  std::lock_guard<std::mutex> lock(state.state);
  if (!peer || state.peer != peer || state.transitioning)
    return false;
  if (ready != state.ready) {
    state.monitor->Recover();
    state.ready = ready;
  }
  return true;
}
} // namespace hm::preview
