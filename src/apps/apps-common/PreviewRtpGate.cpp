#include "PreviewRtpGate.h"

#include <condition_variable>
#include <mutex>
#include <new>
#include <utility>

namespace {
struct State {
  std::mutex flow, state;
  std::shared_ptr<hm::preview::TransportMonitor> monitor;
  std::uint64_t peer{0};
  bool transitioning{false}, ready{false};
  bool requested_ready{false}, pending{false}, stopping{false};
  std::function<bool(bool)> downstream;
  std::condition_variable finished;
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
void reconcile_ready(GstElement* element, gpointer) noexcept {
  auto& state = *get(element)->state;
  try {
    // Do not hold the state lock while waiting: connection notifications must
    // be able to close/invalidate readiness during downstream teardown.
    std::lock_guard<std::mutex> flow(state.flow);
    std::lock_guard<std::mutex> lock(state.state);
    state.ready = false;
    if (!state.stopping && !state.transitioning && state.downstream) {
      state.monitor->Recover();
      if (state.requested_ready)
        state.ready = state.downstream(true);
    }
    state.pending = false;
  } catch (...) {
    std::lock_guard<std::mutex> lock(state.state);
    state.ready = false;
    state.pending = false;
    state.monitor->ProbeFailure();
  }
  state.finished.notify_all();
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
  if (self->state->stopping || peer <= self->state->peer)
    return false;
  // Retire receipt history before touching the old endpoint. Already accepted
  // pushes may still be draining, but can no longer validate a browser receipt.
  if (!self->state->monitor->SetPeer(peer))
    return false;
  self->state->peer = peer;
  self->state->transitioning = true;
  self->state->ready = false;
  self->state->requested_ready = false;
  self->state->downstream = {};
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
  // Raw/encoded frames could arrive while the old endpoint was being stopped.
  state.monitor->Recover();
  if (!mutation())
    return false;
  std::lock_guard<std::mutex> lock(state.state);
  if (state.peer != peer)
    return false;
  state.transitioning = false;
  return true;
}
bool rtp_gate_set_ready(GstElement* element, std::uint64_t peer, bool ready, std::function<bool(bool)> control) {
  auto* self = get(element);
  if (!self)
    return false;
  auto& state = *self->state;
  bool schedule = false;
  {
    std::lock_guard<std::mutex> lock(state.state);
    if (!peer || state.peer != peer || state.transitioning || state.stopping || !control || !control(false))
      return false;
    state.ready = false;
    state.monitor->Recover();
    state.requested_ready = ready;
    state.downstream = std::move(control);
    if (!state.pending) {
      state.pending = true;
      schedule = true;
    }
  }
  if (schedule)
    gst_element_call_async(element, reconcile_ready, nullptr, nullptr);
  return true;
}
bool rtp_gate_is_ready(GstElement* element, std::uint64_t peer) {
  auto* self = get(element);
  if (!self)
    return false;
  std::lock_guard<std::mutex> lock(self->state->state);
  return self->state->peer == peer && self->state->ready && !self->state->transitioning && !self->state->stopping;
}
void rtp_gate_stop(GstElement* element) {
  auto* self = get(element);
  if (!self)
    return;
  auto& state = *self->state;
  std::unique_lock<std::mutex> lock(state.state);
  state.stopping = true;
  state.ready = false;
  state.requested_ready = false;
  state.downstream = {};
  state.finished.wait(lock, [&] { return !state.pending; });
}
} // namespace hm::preview
