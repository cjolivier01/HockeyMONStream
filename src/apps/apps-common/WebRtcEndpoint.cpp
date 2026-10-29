#include "WebRtcEndpoint.h"

#include <gst/webrtc/webrtc.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hstream {
namespace {
struct Track {
  GstElement* valve = nullptr; // Also retained through in-flight peer notifications.
  GstElement* drain = nullptr;
  GstPad* peer_pad = nullptr; // Owned reference.
  GstCaps* caps = nullptr;
  bool eos = false;
};
struct Endpoint {
  std::mutex mutex;
  std::recursive_mutex lifecycle;
  std::string stun;
  guint64 generation = 0;
  bool transitioning = false;
  bool managed_readiness = false;
  bool connected = false;
  std::uint64_t connection_revision = 0;
  GstElement* peer = nullptr; // Owned by endpoint.
  std::vector<Track> tracks;
  ~Endpoint() {
    for (auto& track : tracks) {
      if (track.peer_pad)
        gst_object_unref(track.peer_pad);
      gst_caps_unref(track.caps);
      gst_object_unref(track.valve);
    }
  }
};
constexpr const char* kData = "hstream-webrtc-endpoint";
Endpoint* data(GstElement* element) {
  auto* holder = static_cast<std::shared_ptr<Endpoint>*>(g_object_get_data(G_OBJECT(element), kData));
  return holder ? holder->get() : nullptr;
}
bool configure_sender_clock(GstElement* peer) {
  // Both encoded tracks use the pipeline's presentation running time. A
  // retained peer must use that same clock in its sender reports: wallclock
  // NTP advances through PAUSED while RTP does not, changing the fitted RTP/NTP
  // relation on every Resume. Configure the shared manager before any session
  // is created; rtpbin propagates this setting to all of its sessions.
  GstElement* rtp = gst_bin_get_by_name(GST_BIN(peer), "rtpbin");
  if (!rtp)
    return false;
  GParamSpec* property = g_object_class_find_property(G_OBJECT_GET_CLASS(rtp), "ntp-time-source");
  bool configured = false;
  if (property && G_IS_PARAM_SPEC_ENUM(property)) {
    auto* values = G_ENUM_CLASS(g_type_class_ref(G_PARAM_SPEC_VALUE_TYPE(property)));
    const GEnumValue* running = g_enum_get_value_by_nick(values, "running-time");
    if (running) {
      g_object_set(rtp, "ntp-time-source", running->value, nullptr);
      gint actual = -1;
      g_object_get(rtp, "ntp-time-source", &actual, nullptr);
      configured = actual == running->value;
    }
    g_type_class_unref(values);
  }
  gst_object_unref(rtp);
  return configured;
}
struct InputProbe {
  GWeakRef endpoint;
  size_t index;
};
void drain_eos(GstElement* endpoint, gpointer requested) {
  auto* state = data(endpoint);
  if (!state)
    return;
  std::lock_guard<std::recursive_mutex> lifecycle(state->lifecycle);
  if (GST_STATE(endpoint) < GST_STATE_PAUSED)
    return;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->generation != *static_cast<guint64*>(requested))
      return;
    for (const auto& track : state->tracks)
      if (!track.eos)
        return;
  }
  webrtc_endpoint_detach_peer(endpoint);
}
GstPadProbeReturn input_event(GstPad*, GstPadProbeInfo* info, gpointer user_data) {
  auto* probe = static_cast<InputProbe*>(user_data);
  GstElement* endpoint = GST_ELEMENT(g_weak_ref_get(&probe->endpoint));
  if (!endpoint)
    return GST_PAD_PROBE_OK;
  auto* state = data(endpoint);
  bool drain = false;
  bool all_ended = false;
  guint64 generation = 0;
  GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP)
      state->tracks[probe->index].eos = false;
    if (GST_EVENT_TYPE(event) == GST_EVENT_EOS) {
      state->tracks[probe->index].eos = true;
      drain = state->peer != nullptr || state->transitioning;
      all_ended = true;
      for (const auto& track : state->tracks)
        all_ended = all_ended && track.eos;
      generation = state->generation;
    }
  }
  // EOS must not enter a transport waiting for SDP/DTLS. Move graph changes
  // off the streaming thread, then replay each ended input to its drain.
  if (drain && all_ended)
    gst_element_call_async(
        endpoint, drain_eos, new guint64(generation), [](gpointer p) { delete static_cast<guint64*>(p); });
  gst_object_unref(endpoint);
  return drain ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
}
void connection_changed(GObject* peer, GParamSpec*, gpointer user_data) {
  auto* state = static_cast<std::shared_ptr<Endpoint>*>(user_data)->get();
  std::lock_guard<std::mutex> lock(state->mutex);
  if (G_OBJECT(state->peer) != peer)
    return;
  GstWebRTCPeerConnectionState connection;
  g_object_get(peer, "connection-state", &connection, nullptr);
  state->connected = connection == GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED;
  ++state->connection_revision;
  for (auto& track : state->tracks) {
    g_object_set(track.valve, "drop", state->managed_readiness || !state->connected, nullptr);
  }
}
// Closing the valves first also masks an in-flight push's FLUSHING result
// while NULL releases the old transport. Never hold our mutex across NULL:
// transport teardown can synchronously finish a connection notification.
void remove_peer(GstElement* endpoint, Endpoint* state) {
  GstElement* old;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    old = state->peer;
    state->peer = nullptr;
    state->connected = false;
    ++state->connection_revision;
    state->transitioning = true;
    for (auto& track : state->tracks)
      g_object_set(track.valve, "drop", TRUE, nullptr);
  }
  if (old) {
    g_signal_handlers_disconnect_matched(
        old, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr, reinterpret_cast<gpointer>(connection_changed), nullptr);
    gst_element_set_state(old, GST_STATE_NULL);
  }
  // A detached fakesink can hold the input stream lock in PAUSED preroll.
  // Release every downstream wait before taking that lock to relink the input.
  for (auto& track : state->tracks) {
    if (track.drain)
      gst_element_set_state(track.drain, GST_STATE_NULL);
  }
  for (auto& track : state->tracks) {
    GstPad* src = gst_element_get_static_pad(track.valve, "src");
    GstPad* input = gst_element_get_static_pad(track.valve, "sink");
    GST_PAD_STREAM_LOCK(input);
    GstPad* sink = gst_pad_get_peer(src);
    if (sink) {
      gst_pad_unlink(src, sink);
      gst_object_unref(sink);
    }
    if (track.peer_pad) {
      gst_element_release_request_pad(old, track.peer_pad);
      gst_object_unref(track.peer_pad);
      track.peer_pad = nullptr;
    }
    GST_PAD_STREAM_UNLOCK(input);
    gst_object_unref(input);
    gst_object_unref(src);
    if (track.drain) {
      gst_bin_remove(GST_BIN(endpoint), track.drain);
      track.drain = nullptr;
    }
  }
  if (old)
    gst_bin_remove(GST_BIN(endpoint), old);
}
bool install_drain(GstElement* endpoint, Track& track) {
  track.drain = gst_element_factory_make("fakesink", nullptr);
  if (!track.drain)
    return false;
  g_object_set(track.drain, "sync", FALSE, "async", FALSE, nullptr);
  gst_bin_add(GST_BIN(endpoint), track.drain);
  if (!gst_element_link(track.valve, track.drain))
    return false;
  gst_element_sync_state_with_parent(track.drain);
  g_object_set(track.valve, "drop", FALSE, nullptr);
  return true;
}
} // namespace

GstElement* webrtc_endpoint_create(const char* name, const char* stun_server) {
  GstElement* endpoint = gst_bin_new(name);
  auto* holder = new std::shared_ptr<Endpoint>(std::make_shared<Endpoint>());
  if (stun_server)
    (*holder)->stun = stun_server;
  g_object_set_data_full(
      G_OBJECT(endpoint), kData, holder, [](gpointer p) { delete static_cast<std::shared_ptr<Endpoint>*>(p); });
  return endpoint;
}

GstPad* webrtc_endpoint_add_track(GstElement* endpoint, GstCaps* caps) {
  auto* state = data(endpoint);
  if (!state || state->peer || !caps)
    return nullptr;
  Track track;
  track.valve = gst_element_factory_make("valve", nullptr);
  if (!track.valve)
    return nullptr;
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(track.valve), "drop-mode"))
    g_object_set(track.valve, "drop-mode", 1, nullptr); // forward-sticky-events
  track.caps = gst_caps_ref(caps);
  gst_bin_add(GST_BIN(endpoint), track.valve);
  if (!install_drain(endpoint, track)) {
    if (track.drain)
      gst_bin_remove(GST_BIN(endpoint), track.drain);
    gst_bin_remove(GST_BIN(endpoint), track.valve);
    gst_caps_unref(track.caps);
    return nullptr;
  }
  GstPad* target = gst_element_get_static_pad(track.valve, "sink");
  const auto name = "sink_" + std::to_string(state->tracks.size());
  GstPad* ghost = gst_ghost_pad_new(name.c_str(), target);
  gst_object_unref(target);
  gst_element_add_pad(endpoint, ghost);
  gst_object_ref(track.valve);
  state->tracks.push_back(track);
  auto* probe = new InputProbe;
  g_weak_ref_init(&probe->endpoint, endpoint);
  probe->index = state->tracks.size() - 1;
  GstPad* input = gst_element_get_static_pad(track.valve, "sink");
  gst_pad_add_probe(
      input,
      GstPadProbeType(GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH),
      input_event,
      probe,
      [](gpointer p) {
        auto* probe = static_cast<InputProbe*>(p);
        g_weak_ref_clear(&probe->endpoint);
        delete probe;
      });
  gst_object_unref(input);
  return GST_PAD(gst_object_ref(ghost));
}

bool webrtc_endpoint_prepare_offer(GstElement* endpoint) {
  auto* state = data(endpoint);
  if (!state)
    return false;
  std::lock_guard<std::recursive_mutex> lifecycle(state->lifecycle);
  if (!state->peer || state->tracks.empty())
    return false;
  std::vector<GstCaps*> actual;
  for (const auto& track : state->tracks) {
    GstPad* input = gst_element_get_static_pad(track.valve, "sink");
    GstCaps* caps = gst_pad_get_current_caps(input);
    gst_object_unref(input);
    if (!caps || !gst_caps_is_fixed(caps)) {
      if (caps)
        gst_caps_unref(caps);
      for (auto* retained : actual)
        gst_caps_unref(retained);
      return false;
    }
    actual.push_back(caps);
  }
  bool ready = true;
  for (size_t index = 0; index < state->tracks.size(); ++index) {
    GstWebRTCRTPTransceiver* transceiver = nullptr;
    g_object_get(state->tracks[index].peer_pad, "transceiver", &transceiver, nullptr);
    if (transceiver) {
      g_object_set(transceiver, "codec-preferences", actual[index], nullptr);
      gst_object_unref(transceiver);
    } else
      ready = false;
    gst_caps_unref(actual[index]);
  }
  return ready;
}

void webrtc_endpoint_set_active(GstElement* endpoint, bool active) {
  auto* state = data(endpoint);
  if (!state)
    return;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->managed_readiness)
    return;
  for (auto& track : state->tracks)
    g_object_set(track.valve, "drop", state->peer && !active, nullptr);
}

bool webrtc_endpoint_manage_readiness(GstElement* endpoint) {
  auto* state = data(endpoint);
  if (!state)
    return false;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->peer || state->transitioning)
    return false;
  state->managed_readiness = true;
  return true;
}

bool webrtc_endpoint_connection_snapshot(
    GstElement* endpoint,
    GstElement* peer,
    std::uint64_t* revision,
    bool* connected) {
  auto* state = data(endpoint);
  if (!state || !revision || !connected)
    return false;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (!peer || state->peer != peer || state->transitioning)
    return false;
  *revision = state->connection_revision;
  *connected = state->connected;
  return true;
}

bool webrtc_endpoint_apply_readiness(GstElement* endpoint, GstElement* peer, std::uint64_t revision, bool ready) {
  auto* state = data(endpoint);
  if (!state)
    return false;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (!state->managed_readiness || !peer || state->peer != peer || state->transitioning ||
      state->connection_revision != revision ||
      (ready &&
       (!state->connected || GST_STATE(endpoint) < GST_STATE_PAUSED || GST_STATE_TARGET(endpoint) < GST_STATE_PAUSED)))
    return false;
  for (auto& track : state->tracks)
    g_object_set(track.valve, "drop", !ready, nullptr);
  return true;
}

void webrtc_endpoint_detach_peer(GstElement* endpoint) {
  auto* state = data(endpoint);
  if (!state)
    return;
  std::lock_guard<std::recursive_mutex> lifecycle(state->lifecycle);
  remove_peer(endpoint, state);
  for (auto& track : state->tracks)
    install_drain(endpoint, track);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->transitioning = false;
  }
  for (auto& track : state->tracks) {
    bool ended;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      ended = track.eos;
    }
    if (ended && track.drain) {
      GstPad* sink = gst_element_get_static_pad(track.drain, "sink");
      gst_pad_send_event(sink, gst_event_new_eos());
      gst_object_unref(sink);
    }
  }
}

GstElement* webrtc_endpoint_reset_peer(GstElement* endpoint) {
  auto* state = data(endpoint);
  if (!state)
    return nullptr;
  std::lock_guard<std::recursive_mutex> lifecycle(state->lifecycle);
  remove_peer(endpoint, state);
  GstElement* peer = gst_element_factory_make("webrtcbin", nullptr);
  if (!peer) {
    webrtc_endpoint_detach_peer(endpoint);
    return nullptr;
  }
  if (!configure_sender_clock(peer)) {
    GST_ERROR_OBJECT(endpoint, "WebRTC requires running-time RTCP sender reports for its presentation clock");
    gst_object_unref(peer);
    webrtc_endpoint_detach_peer(endpoint);
    return nullptr;
  }
  g_object_set(peer, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, nullptr);
  if (!state->stun.empty())
    g_object_set(peer, "stun-server", state->stun.c_str(), nullptr);
  gst_bin_add(GST_BIN(endpoint), peer);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->peer = peer;
    state->transitioning = false;
    ++state->generation;
  }
  bool linked = true;
  for (auto& track : state->tracks) {
    GstPadTemplate* templ = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(peer), "sink_%u");
    track.peer_pad = gst_element_request_pad(peer, templ, nullptr, nullptr);
    GstPad* src = gst_element_get_static_pad(track.valve, "src");
    if (!track.peer_pad || gst_pad_link(src, track.peer_pad) != GST_PAD_LINK_OK)
      linked = false;
    gst_object_unref(src);
    if (track.peer_pad) {
      GstWebRTCRTPTransceiver* transceiver = nullptr;
      g_object_get(track.peer_pad, "transceiver", &transceiver, nullptr);
      if (transceiver) {
        g_object_set(transceiver, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, nullptr);
        gst_object_unref(transceiver);
      }
    }
  }
  if (!linked) {
    webrtc_endpoint_detach_peer(endpoint);
    return nullptr;
  }
  auto* holder = static_cast<std::shared_ptr<Endpoint>*>(g_object_get_data(G_OBJECT(endpoint), kData));
  g_signal_connect_data(
      peer,
      "notify::connection-state",
      G_CALLBACK(connection_changed),
      new std::shared_ptr<Endpoint>(*holder),
      [](gpointer p, GClosure*) { delete static_cast<std::shared_ptr<Endpoint>*>(p); },
      GConnectFlags(0));
  if (!gst_element_sync_state_with_parent(peer)) {
    webrtc_endpoint_detach_peer(endpoint);
    return nullptr;
  }
  bool ended = true;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    for (const auto& track : state->tracks)
      ended = ended && track.eos;
  }
  if (ended) {
    webrtc_endpoint_detach_peer(endpoint);
    return nullptr;
  }
  return GST_ELEMENT(gst_object_ref(peer));
}
} // namespace hstream
