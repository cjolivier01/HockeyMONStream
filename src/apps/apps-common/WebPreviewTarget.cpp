#include "WebPreviewTarget.h"

#include "PreviewRtpGate.h"
#include "WebRtcEndpoint.h"

#include <gst/video/video-event.h>
#include <nvbufsurface.h>

#include <climits>
#include <exception>

namespace hm::preview {
namespace {
bool fail(std::string* error, const char* message) {
  if (error)
    *error = message;
  return false;
}

GstPadProbeReturn admit(GstPad* pad, GstPadProbeInfo* info, gpointer data) noexcept {
  try {
    auto& monitor = **static_cast<std::shared_ptr<TransportMonitor>*>(data);
    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    const auto result = monitor.Admit(buffer ? frame_identity(buffer) : nullptr);
    if (!result.accepted)
      return GST_PAD_PROBE_DROP;
    if (result.force_keyframe) {
      // The key-unit count is only a request label, not the 64-bit route/peer
      // identity. The native encoder must emit a real IDR before RTP admission.
      const auto count = static_cast<guint>(monitor.counters().keyframe_requests + 1);
      const bool accepted = gst_pad_send_event(
          pad,
          gst_video_event_new_downstream_force_key_unit(
              GST_BUFFER_PTS(buffer), GST_CLOCK_TIME_NONE, GST_BUFFER_PTS(buffer), TRUE, count));
      monitor.KeyframeRequest(accepted);
      if (!accepted)
        return GST_PAD_PROBE_DROP;
    }
    return GST_PAD_PROBE_OK;
  } catch (...) {
    (**static_cast<std::shared_ptr<TransportMonitor>*>(data)).ProbeFailure();
    return GST_PAD_PROBE_DROP;
  }
}

GstPadProbeReturn encoded(GstPad*, GstPadProbeInfo* info, gpointer data) noexcept {
  try {
    return (**static_cast<std::shared_ptr<TransportMonitor>*>(data)).Encoded(GST_PAD_PROBE_INFO_BUFFER(info))
        ? GST_PAD_PROBE_OK
        : GST_PAD_PROBE_DROP;
  } catch (...) {
    (**static_cast<std::shared_ptr<TransportMonitor>*>(data)).ProbeFailure();
    return GST_PAD_PROBE_DROP;
  }
}

struct PeerGate {
  GstElement* gate;
  GWeakRef endpoint;
  std::uint64_t generation;
  PeerGate(GstElement* gate, GstElement* endpoint, std::uint64_t generation)
      : gate(GST_ELEMENT(gst_object_ref(gate))), generation(generation) {
    g_weak_ref_init(&this->endpoint, endpoint);
  }
  ~PeerGate() {
    g_weak_ref_clear(&endpoint);
    gst_object_unref(gate);
  }
};
struct ReadyDestination {
  GWeakRef endpoint, peer;
  std::uint64_t revision;
  ReadyDestination(GstElement* endpoint, GObject* peer, std::uint64_t revision) : revision(revision) {
    g_weak_ref_init(&this->endpoint, endpoint);
    g_weak_ref_init(&this->peer, peer);
  }
  ~ReadyDestination() {
    g_weak_ref_clear(&endpoint);
    g_weak_ref_clear(&peer);
  }
  bool Apply(bool ready) {
    auto* destination = GST_ELEMENT(g_weak_ref_get(&endpoint));
    auto* current = GST_ELEMENT(g_weak_ref_get(&peer));
    const bool applied =
        destination && current && hstream::webrtc_endpoint_apply_readiness(destination, current, revision, ready);
    if (current)
      gst_object_unref(current);
    if (destination)
      gst_object_unref(destination);
    return applied;
  }
};
void peer_connection(GObject* peer, GParamSpec*, gpointer data) {
  auto* binding = static_cast<PeerGate*>(data);
  auto* endpoint = GST_ELEMENT(g_weak_ref_get(&binding->endpoint));
  if (!endpoint)
    return;
  std::uint64_t revision;
  bool connected;
  if (hstream::webrtc_endpoint_connection_snapshot(endpoint, GST_ELEMENT(peer), &revision, &connected)) {
    auto destination = std::make_shared<ReadyDestination>(endpoint, peer, revision);
    // Managed endpoint notifications close its valves first. The bounded gate
    // worker drains accepted pushes before a revision-matched reopen. Never
    // wait for that flow here: NULL may be joining this notification.
    rtp_gate_set_ready(
        binding->gate, binding->generation, connected, [destination](bool ready) { return destination->Apply(ready); });
  }
  gst_object_unref(endpoint);
}
} // namespace

struct WebPreviewTarget::PeerLifetime {
  std::mutex mutex;
  WebPreviewTarget* target{nullptr};
};

bool WebPreviewTarget::Validate(const WebPreviewConfig& c, std::string* error) {
  if (c.name.empty() || c.stream_id.empty() || c.eof_channel.empty() || c.eof_channel.size() > 64)
    return fail(error, "Preview target requires a name, stream identity and bounded EOF channel");
  if (!c.surface.width || !c.surface.height || c.surface.width > 8192 || c.surface.height > 8192 ||
      c.surface.width % 2 || c.surface.height % 2 || !c.surface.rate_numerator || !c.surface.rate_denominator ||
      c.surface.rate_numerator > INT_MAX || c.surface.rate_denominator > 1000000 ||
      double(c.surface.rate_numerator) / c.surface.rate_denominator > 120 || !c.surface.pool_frames ||
      c.surface.pool_frames > 64 || !c.conversion_pool_frames || !c.bitrate || !c.idr_interval || !c.pending_frames ||
      !c.pending_events || !c.presentation_history || !c.h264_profile || !c.encoder_tuning || !c.stun_server ||
      c.rtp_payload < 96 || c.rtp_payload > 127)
    return fail(error, "Preview target requires explicit valid GPU, encoder, RTP and queue configuration");
  if ((*c.h264_profile != 0 && *c.h264_profile != 1 && *c.h264_profile != 2 && *c.h264_profile != 4) ||
      (*c.encoder_tuning != 2 && *c.encoder_tuning != 3))
    return fail(error, "Preview requires an 8-bit 4:2:0 H.264 profile and a low-latency encoder tuning");
  return true;
}

WebPreviewTarget::WebPreviewTarget(const WebPreviewConfig& config)
    : monitor_(
          std::make_shared<TransportMonitor>(
              config.pending_frames,
              config.pending_events,
              config.presentation_history)),
      peer_lifetime_(std::make_shared<PeerLifetime>()) {
  peer_lifetime_->target = this;
}

std::unique_ptr<WebPreviewTarget> WebPreviewTarget::Create(
    const WebPreviewConfig& config,
    SurfacePainter painter,
    std::string* error) {
  if (error)
    error->clear();
  if (!Validate(config, error))
    return nullptr;
  try {
    auto target = std::unique_ptr<WebPreviewTarget>(new WebPreviewTarget(config));
    if (!target->Build(config, std::move(painter), error))
      return nullptr;
    return target;
  } catch (const std::exception& exception) {
    if (error)
      *error = exception.what();
    return nullptr;
  }
}

bool WebPreviewTarget::Probe(
    GstElement* element,
    const char* name,
    GstPadProbeType type,
    GstPadProbeCallback callback) {
  auto* pad = gst_element_get_static_pad(element, name);
  if (!pad)
    return false;
  auto* hold = new std::shared_ptr<TransportMonitor>(monitor_);
  const auto id = gst_pad_add_probe(
      pad, type, callback, hold, [](gpointer data) { delete static_cast<std::shared_ptr<TransportMonitor>*>(data); });
  if (!id) {
    gst_object_unref(pad);
    return false;
  }
  probes_.emplace_back(pad, id);
  return true;
}

bool WebPreviewTarget::Build(const WebPreviewConfig& c, SurfacePainter painter, std::string* error) {
  bin_ = gst_bin_new(c.name.c_str());
  if (!bin_)
    return fail(error, "Could not allocate preview target bin");
  gst_object_ref_sink(bin_);
  auto add = [&](GstElement* element) -> GstElement* {
    if (!element)
      return nullptr;
    if (!gst_bin_add(GST_BIN(bin_), element)) {
      gst_object_unref(element);
      return nullptr;
    }
    return element;
  };
  selected_ = add(selected_stream_create("selected", c.stream_id.c_str(), "memory:NVMM", c.eof_channel.c_str()));
  auto* surface = add(gpu_surface_create("surface", c.surface, std::move(painter)));
  auto* convert = add(gst_element_factory_make("nvvideoconvert", "convert"));
  auto* raw = add(gst_element_factory_make("capsfilter", "encoder-input"));
  auto* encoder = add(gst_element_factory_make("nvv4l2h264enc", "encoder"));
  auto* parser = add(gst_element_factory_make("h264parse", "parser"));
  auto* au = add(gst_element_factory_make("capsfilter", "access-units"));
  auto* payloader = add(gst_element_factory_make("rtph264pay", "rtp"));
  rtp_gate_ = add(rtp_gate_create("rtp-gate", monitor_));
  if (!selected_ || !surface || !convert || !raw || !encoder || !parser || !au || !payloader || !rtp_gate_)
    return fail(error, "Required GPU H.264 preview element is unavailable; no software fallback exists");
  for (const char* property :
       {"bitrate",
        "profile",
        "iframeinterval",
        "idrinterval",
        "num-B-Frames",
        "tuning-info-id",
        "insert-sps-pps",
        "gpu-id"}) {
    if (!g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), property))
      return fail(error, "GPU H.264 encoder lacks a required low-latency property");
  }
  g_object_set(
      convert,
      "gpu-id",
      c.surface.gpu_id,
      "nvbuf-memory-type",
      NVBUF_MEM_CUDA_DEVICE,
      "compute-hw",
      1,
      "output-buffers",
      c.conversion_pool_frames,
      nullptr);
  auto* caps = gst_caps_new_simple(
      "video/x-raw",
      "format",
      G_TYPE_STRING,
      "NV12",
      "width",
      G_TYPE_INT,
      c.surface.width,
      "height",
      G_TYPE_INT,
      c.surface.height,
      nullptr);
  gst_caps_set_features(caps, 0, gst_caps_features_new("memory:NVMM", nullptr));
  g_object_set(raw, "caps", caps, nullptr);
  gst_caps_unref(caps);
  caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
  g_object_set(au, "caps", caps, nullptr);
  gst_caps_unref(caps);
  g_object_set(
      encoder,
      "bitrate",
      c.bitrate,
      "profile",
      *c.h264_profile,
      "iframeinterval",
      c.idr_interval,
      "idrinterval",
      c.idr_interval,
      "num-B-Frames",
      0u,
      "tuning-info-id",
      *c.encoder_tuning,
      "insert-sps-pps",
      TRUE,
      "gpu-id",
      c.surface.gpu_id,
      nullptr);
  g_object_set(payloader, "pt", c.rtp_payload, "config-interval", -1, nullptr);
  if (!gst_element_link_many(selected_, surface, convert, raw, encoder, parser, au, payloader, rtp_gate_, nullptr))
    return fail(error, "Could not link GPU preview encoder path");
  caps = gst_caps_new_simple(
      "application/x-rtp",
      "media",
      G_TYPE_STRING,
      "video",
      "encoding-name",
      G_TYPE_STRING,
      "H264",
      "payload",
      G_TYPE_INT,
      c.rtp_payload,
      "clock-rate",
      G_TYPE_INT,
      90000,
      nullptr);
  auto* endpoint = hstream::webrtc_endpoint_create("endpoint", c.stun_server->c_str());
  auto* input = endpoint ? hstream::webrtc_endpoint_add_track(endpoint, caps) : nullptr;
  gst_caps_unref(caps);
  // Add its initial drain before parenting the endpoint. GstBin snapshots a
  // child's SINK flag when adding it; an empty nested endpoint otherwise leaves
  // this target absent from the parent pipeline's EOS aggregation.
  endpoint_ = add(endpoint);
  if (!endpoint_ || !hstream::webrtc_endpoint_manage_readiness(endpoint_)) {
    if (input)
      gst_object_unref(input);
    return fail(error, "Could not bind managed preview endpoint readiness");
  }
  auto* output = gst_element_get_static_pad(rtp_gate_, "src");
  const bool linked = endpoint_ && input && output && gst_pad_link(output, input) == GST_PAD_LINK_OK;
  if (input)
    gst_object_unref(input);
  if (output)
    gst_object_unref(output);
  if (!linked || !Probe(encoder, "sink", GST_PAD_PROBE_TYPE_BUFFER, admit) ||
      !Probe(au, "src", GST_PAD_PROBE_TYPE_BUFFER, encoded))
    return fail(error, "Could not attach GPU preview transport and identity probes");
  return true;
}

WebPreviewTarget::~WebPreviewTarget() {
  {
    std::lock_guard<std::mutex> lock(peer_lifetime_->mutex);
    peer_lifetime_->target = nullptr;
  }
  monitor_->Recover();
  if (rtp_gate_)
    rtp_gate_stop(rtp_gate_);
  for (auto& [pad, id] : probes_) {
    gst_pad_remove_probe(pad, id);
    gst_object_unref(pad);
  }
  while (!routes_.empty())
    RemoveRoute(routes_.begin()->first);
  if (bin_)
    gst_object_unref(bin_);
}

GstPad* WebPreviewTarget::AddRoute(const std::string& channel) {
  if (channel.empty() || channel.size() > 64)
    return nullptr;
  auto* selected = selected_stream_add_route(selected_, channel.c_str());
  if (!selected)
    return nullptr;
  const auto name = "sink_" + std::to_string(next_route_++);
  auto* ghost = gst_ghost_pad_new(name.c_str(), selected);
  if (!ghost || !gst_element_add_pad(bin_, ghost)) {
    if (ghost)
      gst_object_unref(ghost);
    selected_stream_remove_route(selected_, selected);
    gst_object_unref(selected);
    return nullptr;
  }
  routes_.emplace(ghost, Route{selected, channel});
  if (GST_STATE(bin_) >= GST_STATE_PAUSED)
    gst_pad_set_active(ghost, TRUE);
  return GST_PAD(gst_object_ref(ghost));
}

void WebPreviewTarget::RemoveRoute(GstPad* route) {
  auto found = routes_.find(route);
  if (found == routes_.end())
    return;
  if (active_route_ == route) {
    // Removing an active source must retire its delayed encoder output even
    // when the replacement has not been constructed or never produces a frame.
    selected_stream_select(selected_, nullptr, operation_);
    monitor_->Select("", operation_);
    active_route_ = nullptr;
  }
  selected_stream_remove_route(selected_, found->second.selected);
  gst_ghost_pad_set_target(GST_GHOST_PAD(route), nullptr);
  gst_element_remove_pad(bin_, route);
  gst_object_unref(found->second.selected);
  routes_.erase(found);
}

bool WebPreviewTarget::Select(GstPad* route, std::uint64_t operation) {
  auto found = routes_.find(route);
  if (route && found == routes_.end())
    return false;
  if (!selected_stream_select(selected_, route ? found->second.selected : nullptr, operation))
    return false;
  if (!monitor_->Select(route ? found->second.channel : "", operation))
    return false;
  active_route_ = route;
  operation_ = operation;
  return true;
}

bool WebPreviewTarget::Active(GstPad* route, std::uint64_t operation) const {
  const auto found = routes_.find(route);
  return found != routes_.end() && selected_stream_active(selected_, found->second.selected, operation);
}

GstElement* WebPreviewTarget::ReplacePeer(std::uint64_t generation) {
  if (!rtp_gate_begin_transition(rtp_gate_, generation))
    return nullptr;
  peer_generation_ = generation;
  // Release preroll/transport waits before the output barrier. Taking the
  // barrier first would deadlock a push that needs this NULL to complete.
  hstream::webrtc_endpoint_detach_peer(endpoint_);
  GstElement* peer = nullptr;
  const bool replaced = rtp_gate_finish_transition(rtp_gate_, generation, [&] {
    peer = hstream::webrtc_endpoint_reset_peer(endpoint_);
    if (!peer)
      return false;
    g_signal_connect_data(
        peer,
        "notify::connection-state",
        G_CALLBACK(peer_connection),
        new PeerGate(rtp_gate_, endpoint_, generation),
        [](gpointer data, GClosure*) { delete static_cast<PeerGate*>(data); },
        GConnectFlags(0));
    return true;
  });
  if (!replaced) {
    if (peer)
      gst_object_unref(peer);
    return nullptr;
  }
  PeerGate current(rtp_gate_, endpoint_, generation);
  peer_connection(G_OBJECT(peer), nullptr, &current);
  return peer;
}

bool WebPreviewTarget::DetachPeer(std::uint64_t generation) {
  if (!rtp_gate_begin_transition(rtp_gate_, generation))
    return false;
  peer_generation_ = generation;
  hstream::webrtc_endpoint_detach_peer(endpoint_);
  return rtp_gate_finish_transition(rtp_gate_, generation, [] { return true; }) &&
      rtp_gate_set_ready(rtp_gate_, generation, true, [](bool) { return true; });
}

std::shared_ptr<WebPreviewSignaling> WebPreviewTarget::Signaling(const WebPreviewObserverConfig& config) {
  if (!signaling_.expired())
    return {};
  auto result = std::make_shared<WebPreviewSignaling>();
  result->observer = std::make_shared<WebPreviewObserver>(monitor_, config);
  result->initial_peer_generation = peer_generation_;
  std::weak_ptr<PeerLifetime> weak = peer_lifetime_;
  result->replace_peer = [weak](std::uint64_t peer) -> GstElement* {
    auto lifetime = weak.lock();
    if (!lifetime)
      return nullptr;
    std::lock_guard<std::mutex> lock(lifetime->mutex);
    return lifetime->target ? lifetime->target->ReplacePeer(peer) : nullptr;
  };
  result->detach_peer = [weak](std::uint64_t peer) {
    auto lifetime = weak.lock();
    if (!lifetime)
      return false;
    std::lock_guard<std::mutex> lock(lifetime->mutex);
    return lifetime->target && lifetime->target->DetachPeer(peer);
  };
  signaling_ = result;
  return result;
}
std::vector<TransportEvent> WebPreviewTarget::TakeEvents() {
  return monitor_->TakeEvents();
}
TransportCounters WebPreviewTarget::counters() const {
  return monitor_->counters();
}
} // namespace hm::preview
