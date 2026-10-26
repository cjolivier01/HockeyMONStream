/*
 * SPDX-FileCopyrightText: Copyright (c) 2018-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#include "WebRtcSignaling.h"
#include "WebRtcEndpoint.h"

#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <libsoup/soup.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
const char kWebRtcClientHtml[] = R"html(<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>HStream WebRTC Preview</title>
  <style>
    html, body {
      background: #111;
      color: #eee;
      font-family: system-ui, sans-serif;
      height: 100%;
      margin: 0;
    }
    body {
      display: grid;
      grid-template-rows: 1fr auto;
    }
    video {
      background: #000;
      height: 100%;
      object-fit: contain;
      width: 100%;
    }
    #status {
      font-size: 14px;
      padding: 10px 12px;
    }
  </style>
</head>
<body>
  <video id="video" autoplay playsinline controls></video>
  <div id="status">Connecting...</div>
  <script>
    const video = document.getElementById('video');
    const status = document.getElementById('status');
    const ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`);
    const pc = new RTCPeerConnection();
    const inboundStream = new MediaStream();
    video.srcObject = inboundStream;

    const pendingCandidates = [];
    pc.ontrack = (event) => {
      inboundStream.addTrack(event.track);
      status.textContent = 'Connected';
    };
    pc.onconnectionstatechange = () => {
      status.textContent = `WebRTC ${pc.connectionState}`;
    };
    pc.onicecandidate = (event) => {
      if (!event.candidate || ws.readyState !== WebSocket.OPEN) return;
      ws.send(JSON.stringify({
        type: 'candidate',
        sdpMLineIndex: event.candidate.sdpMLineIndex,
        candidate: event.candidate.candidate
      }));
    };
    ws.onopen = async () => {
      status.textContent = 'Negotiating...';
    };
    ws.onmessage = async (event) => {
      const message = JSON.parse(event.data);
      if (message.type === 'offer') {
        await pc.setRemoteDescription({type: 'offer', sdp: message.sdp});
        while (pendingCandidates.length) await pc.addIceCandidate(pendingCandidates.shift());
        const answer = await pc.createAnswer();
        await pc.setLocalDescription(answer);
        ws.send(JSON.stringify({type: 'answer', sdp: pc.localDescription.sdp}));
      } else if (message.type === 'candidate' && message.candidate) {
        const candidate = {candidate: message.candidate, sdpMLineIndex: message.sdpMLineIndex};
        if (pc.remoteDescription) await pc.addIceCandidate(candidate);
        else pendingCandidates.push(candidate);
      } else if (message.type === 'error') {
        status.textContent = message.message;
      }
    };
    ws.onerror = () => {
      status.textContent = 'WebSocket error';
    };
    ws.onclose = () => {
      if (pc.connectionState !== 'connected') status.textContent = 'Disconnected';
    };
  </script>
</body>
</html>
)html";

struct WebRtcSignalServer : std::enable_shared_from_this<WebRtcSignalServer> {
  std::recursive_mutex mutex;
  GstElement* endpoint{nullptr};
  GstElement* webrtc{nullptr};
  SoupServer* server{nullptr};
  SoupWebsocketConnection* connection{nullptr};
  GMainContext* context{g_main_context_ref_thread_default()};
  guint64 generation{0};
  GSource* offer_source{nullptr};
  GSource* frame_source{nullptr};
  std::shared_ptr<hm::preview::WebPreviewSignaling> preview;
  bool offer_sent{false};
  bool answer_received{false};
  bool remote_ready{false};
  std::vector<std::pair<guint, std::string>> pending_candidates;
  bool stopped{false};
  guint port{0};
  ~WebRtcSignalServer() {
    if (webrtc)
      gst_object_unref(webrtc);
    if (endpoint)
      gst_object_unref(endpoint);
    g_main_context_unref(context);
  }
};

std::vector<std::shared_ptr<WebRtcSignalServer>> webrtc_signal_servers;
GMutex webrtc_signal_servers_lock;

struct WebRtcPeerCallback {
  std::shared_ptr<WebRtcSignalServer> server;
  guint64 generation;
  GstElement* webrtc;
  explicit WebRtcPeerCallback(WebRtcSignalServer* state)
      : server(state->shared_from_this()),
        generation(state->generation),
        webrtc(GST_ELEMENT(gst_object_ref(state->webrtc))) {}
  ~WebRtcPeerCallback() {
    gst_object_unref(webrtc);
  }
};

// GstPromise and ICE callbacks run on streaming threads. All Soup operations and
// peer mutations run on the signaling context, with the originating peer fenced.
void dispatch_webrtc_peer(WebRtcPeerCallback* peer, std::function<void(WebRtcSignalServer*)> callback) {
  auto state = peer->server;
  const guint64 generation = peer->generation;
  auto* task = new std::function<void()>([state, generation, callback = std::move(callback)] {
    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    if (!state->stopped && state->connection && state->generation == generation)
      callback(state.get());
  });
  GSource* source = g_idle_source_new();
  g_source_set_callback(
      source,
      [](gpointer data) -> gboolean {
        (*static_cast<std::function<void()>*>(data))();
        return G_SOURCE_REMOVE;
      },
      task,
      [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
  g_source_attach(source, state->context);
  g_source_unref(source);
}

const gchar* json_string_member(JsonObject* object, const gchar* member) {
  if (!json_object_has_member(object, member)) {
    return nullptr;
  }
  JsonNode* node = json_object_get_member(object, member);
  if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING) {
    return nullptr;
  }
  return json_object_get_string_member(object, member);
}

gint json_int_member(JsonObject* object, const gchar* member, gint default_value) {
  if (!json_object_has_member(object, member)) {
    return default_value;
  }
  JsonNode* node = json_object_get_member(object, member);
  if (!JSON_NODE_HOLDS_VALUE(node)) {
    return default_value;
  }
  return json_object_get_int_member(object, member);
}

void send_websocket_text(WebRtcSignalServer* signal_server, const gchar* text) {
  if (signal_server->connection &&
      soup_websocket_connection_get_state(signal_server->connection) == SOUP_WEBSOCKET_STATE_OPEN) {
    soup_websocket_connection_send_text(signal_server->connection, text);
  }
}

void send_json_message(JsonBuilder* builder, WebRtcSignalServer* signal_server) {
  JsonGenerator* generator = json_generator_new();
  JsonNode* root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);
  gchar* text = json_generator_to_data(generator, nullptr);
  send_websocket_text(signal_server, text);
  g_free(text);
  json_node_free(root);
  g_object_unref(generator);
}

void start_preview_observer(const std::shared_ptr<WebRtcSignalServer>& state) {
  if (!state->preview)
    return;
  state->frame_source = g_timeout_source_new(state->preview->observer->poll_interval_ms());
  g_source_set_callback(
      state->frame_source,
      [](gpointer data) -> gboolean {
        auto state = static_cast<std::weak_ptr<WebRtcSignalServer>*>(data)->lock();
        if (!state)
          return G_SOURCE_REMOVE;
        std::lock_guard<std::recursive_mutex> lock(state->mutex);
        if (state->stopped)
          return G_SOURCE_REMOVE;
        const auto message = state->preview->observer->Poll();
        if (message && state->connection)
          send_websocket_text(state.get(), message->c_str());
        return G_SOURCE_CONTINUE;
      },
      new std::weak_ptr<WebRtcSignalServer>(state),
      [](gpointer data) { delete static_cast<std::weak_ptr<WebRtcSignalServer>*>(data); });
  g_source_attach(state->frame_source, state->context);
}

void send_error_message(WebRtcSignalServer* signal_server, const gchar* message) {
  JsonBuilder* builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "error");
  json_builder_set_member_name(builder, "message");
  json_builder_add_string_value(builder, message);
  json_builder_end_object(builder);
  send_json_message(builder, signal_server);
  g_object_unref(builder);
}

void send_offer_message(WebRtcSignalServer* signal_server, const gchar* sdp) {
  JsonBuilder* builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "offer");
  json_builder_set_member_name(builder, "sdp");
  json_builder_add_string_value(builder, sdp);
  json_builder_end_object(builder);
  send_json_message(builder, signal_server);
  g_object_unref(builder);
}

void send_candidate_message(WebRtcSignalServer* signal_server, guint mline_index, const gchar* candidate) {
  JsonBuilder* builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "candidate");
  json_builder_set_member_name(builder, "sdpMLineIndex");
  json_builder_add_int_value(builder, mline_index);
  json_builder_set_member_name(builder, "candidate");
  json_builder_add_string_value(builder, candidate);
  json_builder_end_object(builder);
  send_json_message(builder, signal_server);
  g_object_unref(builder);
}

void on_webrtc_ice_candidate(GstElement*, guint mline_index, gchar* candidate, gpointer user_data) {
  auto* peer = static_cast<WebRtcPeerCallback*>(user_data);
  const std::string value = candidate ? candidate : "";
  dispatch_webrtc_peer(peer, [mline_index, value](WebRtcSignalServer* state) {
    send_candidate_message(state, mline_index, value.c_str());
  });
}

void on_webrtc_offer_created(GstPromise* promise, gpointer user_data) {
  auto* peer = static_cast<WebRtcPeerCallback*>(user_data);
  const GstStructure* reply =
      gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED ? gst_promise_get_reply(promise) : nullptr;
  GstWebRTCSessionDescription* offer = nullptr;
  if (reply)
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
  std::shared_ptr<GstWebRTCSessionDescription> owned(offer, [](GstWebRTCSessionDescription* value) {
    if (value)
      gst_webrtc_session_description_free(value);
  });
  dispatch_webrtc_peer(peer, [owned](WebRtcSignalServer* state) {
    if (!owned) {
      send_error_message(state, "Failed to create WebRTC offer");
      return;
    }
    g_signal_emit_by_name(state->webrtc, "set-local-description", owned.get(), NULL);
    gchar* sdp = gst_sdp_message_as_text(owned->sdp);
    if (sdp) {
      state->offer_sent = true;
      send_offer_message(state, sdp);
      g_free(sdp);
    }
  });
  gst_promise_unref(promise);
}

void on_webrtc_remote_description_set(GstPromise* promise, gpointer user_data) {
  auto* peer = static_cast<WebRtcPeerCallback*>(user_data);
  const GstStructure* reply =
      gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED ? gst_promise_get_reply(promise) : nullptr;
  const bool failed =
      gst_promise_wait(promise) != GST_PROMISE_RESULT_REPLIED || (reply && gst_structure_has_field(reply, "error"));
  dispatch_webrtc_peer(peer, [failed](WebRtcSignalServer* state) {
    if (failed) {
      send_error_message(state, "Failed to apply browser SDP answer");
      return;
    }
    state->remote_ready = true;
    for (const auto& candidate : state->pending_candidates) {
      g_signal_emit_by_name(state->webrtc, "add-ice-candidate", candidate.first, candidate.second.c_str());
    }
    state->pending_candidates.clear();
  });
  gst_promise_unref(promise);
}

void handle_webrtc_answer(WebRtcSignalServer* state, const gchar* sdp_text) {
  if (!state->offer_sent || state->answer_received) {
    send_error_message(state, "Unexpected WebRTC answer");
    return;
  }
  GstSDPMessage* sdp = nullptr;
  if (gst_sdp_message_new(&sdp) != GST_SDP_OK) {
    send_error_message(state, "Failed to allocate SDP message");
    return;
  }
  if (gst_sdp_message_parse_buffer(reinterpret_cast<const guint8*>(sdp_text), strlen(sdp_text), sdp) != GST_SDP_OK) {
    gst_sdp_message_free(sdp);
    send_error_message(state, "Failed to parse browser SDP answer");
    return;
  }
  state->answer_received = true;
  GstWebRTCSessionDescription* answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
  GstPromise* promise = gst_promise_new_with_change_func(
      on_webrtc_remote_description_set, new WebRtcPeerCallback(state), [](gpointer data) {
        delete static_cast<WebRtcPeerCallback*>(data);
      });
  g_signal_emit_by_name(state->webrtc, "set-remote-description", answer, promise);
  gst_webrtc_session_description_free(answer);
}

void schedule_webrtc_offer(WebRtcSignalServer* state) {
  // Calibration may not have produced encoded media yet. Wait for actual caps
  // instead of advertising guessed H.264 profile/level or browser payload IDs.
  state->offer_source = g_timeout_source_new(200);
  g_source_set_callback(
      state->offer_source,
      [](gpointer data) -> gboolean {
        auto* peer = static_cast<WebRtcPeerCallback*>(data);
        auto state = peer->server;
        std::lock_guard<std::recursive_mutex> lock(state->mutex);
        if (state->stopped || !state->connection || state->generation != peer->generation)
          return G_SOURCE_REMOVE;
        if (!hstream::webrtc_endpoint_prepare_offer(state->endpoint))
          return G_SOURCE_CONTINUE;
        GstPromise* promise = gst_promise_new_with_change_func(
            on_webrtc_offer_created, new WebRtcPeerCallback(state.get()), [](gpointer value) {
              delete static_cast<WebRtcPeerCallback*>(value);
            });
        GSource* source = state->offer_source;
        state->offer_source = nullptr;
        g_source_unref(source);
        g_signal_emit_by_name(state->webrtc, "create-offer", NULL, promise);
        return G_SOURCE_REMOVE;
      },
      new WebRtcPeerCallback(state),
      [](gpointer data) { delete static_cast<WebRtcPeerCallback*>(data); });
  g_source_attach(state->offer_source, state->context);
}

void handle_webrtc_candidate(WebRtcSignalServer* signal_server, JsonObject* object) {
  const gchar* candidate = json_string_member(object, "candidate");
  if (!candidate) {
    return;
  }
  const gint mline_index = json_int_member(object, "sdpMLineIndex", 0);
  if (mline_index < 0 || strlen(candidate) > 4096)
    return;
  if (!signal_server->remote_ready) {
    if (signal_server->pending_candidates.size() < 256)
      signal_server->pending_candidates.emplace_back(mline_index, candidate);
    return;
  }
  g_signal_emit_by_name(signal_server->webrtc, "add-ice-candidate", mline_index, candidate);
}

void on_webrtc_ws_message(
    SoupWebsocketConnection* connection,
    SoupWebsocketDataType type,
    GBytes* message,
    gpointer user_data) {
  auto owned = *static_cast<std::shared_ptr<WebRtcSignalServer>*>(user_data);
  auto* signal_server = owned.get();
  std::lock_guard<std::recursive_mutex> lock(signal_server->mutex);
  if (signal_server->stopped || signal_server->connection != connection || !signal_server->webrtc)
    return;
  if (type != SOUP_WEBSOCKET_DATA_TEXT) {
    return;
  }

  gsize size = 0;
  const gchar* data = static_cast<const gchar*>(g_bytes_get_data(message, &size));
  JsonParser* parser = json_parser_new();
  GError* error = nullptr;
  if (!json_parser_load_from_data(parser, data, size, &error)) {
    if (error) {
      g_printerr("WebRTC signaling JSON parse failed: %s\n", error->message);
      g_error_free(error);
    }
    send_error_message(signal_server, "Invalid signaling JSON");
    g_object_unref(parser);
    return;
  }

  JsonNode* root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    send_error_message(signal_server, "Signaling message must be a JSON object");
    g_object_unref(parser);
    return;
  }
  JsonObject* object = json_node_get_object(root);
  const gchar* message_type = json_string_member(object, "type");
  if (!message_type) {
    send_error_message(signal_server, "Signaling message is missing type");
  } else if (!g_strcmp0(message_type, "answer")) {
    const gchar* sdp = json_string_member(object, "sdp");
    if (sdp) {
      handle_webrtc_answer(signal_server, sdp);
    } else {
      send_error_message(signal_server, "Answer is missing SDP");
    }
  } else if (!g_strcmp0(message_type, "candidate")) {
    handle_webrtc_candidate(signal_server, object);
  } else if (
      signal_server->preview && size <= 4096 &&
      signal_server->preview->observer->Handle(object) != hm::preview::WebPreviewObserver::Message::kUnrecognized) {
    // Receipt validity is observable in the bounded observer/monitor counters.
    // Do not reflect arbitrary receipt traffic into another outgoing queue.
  } else {
    send_error_message(signal_server, "Expected an SDP answer or ICE candidate; refresh the preview client");
  }
  g_object_unref(parser);
}

void detach_webrtc_peer(WebRtcSignalServer* state) {
  ++state->generation;
  if (state->preview)
    state->preview->observer->Retire(state->generation);
  state->offer_sent = false;
  state->answer_received = false;
  state->remote_ready = false;
  state->pending_candidates.clear();
  if (state->offer_source) {
    g_source_destroy(state->offer_source);
    g_source_unref(state->offer_source);
    state->offer_source = nullptr;
  }
  if (state->webrtc) {
    g_signal_handlers_disconnect_matched(
        state->webrtc,
        G_SIGNAL_MATCH_FUNC,
        0,
        0,
        nullptr,
        reinterpret_cast<gpointer>(on_webrtc_ice_candidate),
        nullptr);
    if (!state->preview)
      hstream::webrtc_endpoint_detach_peer(state->endpoint);
    gst_object_unref(state->webrtc);
    state->webrtc = nullptr;
  }
  if (state->preview)
    state->preview->detach_peer(state->generation);
}

void on_webrtc_ws_closed(SoupWebsocketConnection* connection, gpointer user_data) {
  auto owned = *static_cast<std::shared_ptr<WebRtcSignalServer>*>(user_data);
  auto* state = owned.get();
  std::lock_guard<std::recursive_mutex> lock(state->mutex);
  if (state->connection != connection)
    return;
  g_signal_handlers_disconnect_matched(
      connection, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr, reinterpret_cast<gpointer>(on_webrtc_ws_message), nullptr);
  g_signal_handlers_disconnect_matched(
      connection, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr, reinterpret_cast<gpointer>(on_webrtc_ws_closed), nullptr);
  state->connection = nullptr;
  g_object_unref(connection);
  detach_webrtc_peer(state);
}

void on_webrtc_ws_connected(
    SoupServer* server,
#if SOUP_MAJOR_VERSION >= 3
    SoupServerMessage* message,
    const char* path,
    SoupWebsocketConnection* connection,
#else
    SoupWebsocketConnection* connection,
    const char* path,
    SoupClientContext* client,
#endif
    gpointer user_data) {
  (void)server;
  (void)path;
#if SOUP_MAJOR_VERSION >= 3
  (void)message;
#else
  (void)client;
#endif
  auto owned = *static_cast<std::shared_ptr<WebRtcSignalServer>*>(user_data);
  auto* signal_server = owned.get();
  std::lock_guard<std::recursive_mutex> lock(signal_server->mutex);
  if (signal_server->stopped)
    return;

  // SDP can be larger than a receipt, but both are bounded before JSON parsing.
  // This is a protocol admission limit for the optional preview adapter.
  if (signal_server->preview)
    soup_websocket_connection_set_max_incoming_payload_size(connection, 256 * 1024);

  if (signal_server->connection) {
    auto* old = signal_server->connection;
    signal_server->connection = nullptr;
    g_signal_handlers_disconnect_matched(
        old, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr, reinterpret_cast<gpointer>(on_webrtc_ws_message), nullptr);
    g_signal_handlers_disconnect_matched(
        old, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr, reinterpret_cast<gpointer>(on_webrtc_ws_closed), nullptr);
    soup_websocket_connection_close(old, SOUP_WEBSOCKET_CLOSE_NORMAL, "replaced");
    g_object_unref(old);
  }
  detach_webrtc_peer(signal_server);
  if (signal_server->preview) {
    ++signal_server->generation;
    signal_server->preview->observer->Retire(signal_server->generation);
    signal_server->webrtc = signal_server->preview->replace_peer(signal_server->generation);
    if (signal_server->webrtc)
      signal_server->preview->observer->Activate(signal_server->generation);
  } else {
    signal_server->webrtc = hstream::webrtc_endpoint_reset_peer(signal_server->endpoint);
  }
  if (!signal_server->webrtc) {
    soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_SERVER_ERROR, "preview initialization failed");
    return;
  }
  signal_server->connection = SOUP_WEBSOCKET_CONNECTION(g_object_ref(connection));
  g_signal_connect_data(
      signal_server->webrtc,
      "on-ice-candidate",
      G_CALLBACK(on_webrtc_ice_candidate),
      new WebRtcPeerCallback(signal_server),
      [](gpointer data, GClosure*) { delete static_cast<WebRtcPeerCallback*>(data); },
      static_cast<GConnectFlags>(0));
  for (const auto& entry :
       {std::make_pair("message", G_CALLBACK(on_webrtc_ws_message)),
        std::make_pair("closed", G_CALLBACK(on_webrtc_ws_closed))}) {
    g_signal_connect_data(
        connection,
        entry.first,
        entry.second,
        new std::shared_ptr<WebRtcSignalServer>(owned),
        [](gpointer data, GClosure*) { delete static_cast<std::shared_ptr<WebRtcSignalServer>*>(data); },
        static_cast<GConnectFlags>(0));
  }
  schedule_webrtc_offer(signal_server);
}

void on_webrtc_http_request(
    SoupServer* server,
#if SOUP_MAJOR_VERSION >= 3
    SoupServerMessage* msg,
#else
    SoupMessage* msg,
#endif
    const char* path,
    GHashTable* query,
#if SOUP_MAJOR_VERSION < 3
    SoupClientContext* client,
#endif
    gpointer user_data) {
  (void)server;
  (void)query;
#if SOUP_MAJOR_VERSION < 3
  (void)client;
#endif
  (void)user_data;
  if (g_strcmp0(path, "/") && g_strcmp0(path, "/index.html")) {
#if SOUP_MAJOR_VERSION >= 3
    soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, nullptr);
#else
    soup_message_set_status(msg, SOUP_STATUS_NOT_FOUND);
#endif
    return;
  }
#if SOUP_MAJOR_VERSION >= 3
  soup_server_message_set_response(
      msg, "text/html; charset=utf-8", SOUP_MEMORY_STATIC, kWebRtcClientHtml, strlen(kWebRtcClientHtml));
  soup_server_message_set_status(msg, SOUP_STATUS_OK, nullptr);
#else
  soup_message_set_response(
      msg, "text/html; charset=utf-8", SOUP_MEMORY_STATIC, kWebRtcClientHtml, strlen(kWebRtcClientHtml));
  soup_message_set_status(msg, SOUP_STATUS_OK);
#endif
}

} // namespace

namespace hm::webrtc {
bool Start(GstElement* webrtc, unsigned port, std::shared_ptr<hm::preview::WebPreviewSignaling> preview) {
  GError* error = nullptr;
  auto owned = std::make_shared<WebRtcSignalServer>();
  auto* signal_server = owned.get();
  signal_server->endpoint = GST_ELEMENT(gst_object_ref(webrtc));
  signal_server->preview = std::move(preview);
  if (signal_server->preview)
    signal_server->generation = signal_server->preview->initial_peer_generation;
  signal_server->port = port;
  signal_server->server = soup_server_new("server-header", "hstream-webrtc", NULL);
  if (!signal_server->server) {
    g_printerr("Failed to create WebRTC signaling server\n");
    return FALSE;
  }

  soup_server_add_handler(signal_server->server, "/", on_webrtc_http_request, signal_server, NULL);
  soup_server_add_handler(signal_server->server, "/index.html", on_webrtc_http_request, signal_server, NULL);
  soup_server_add_websocket_handler(
      signal_server->server,
      "/ws",
      NULL,
      NULL,
      on_webrtc_ws_connected,
      new std::shared_ptr<WebRtcSignalServer>(owned),
      [](gpointer data) { delete static_cast<std::shared_ptr<WebRtcSignalServer>*>(data); });
  if (!soup_server_listen_all(
          signal_server->server, signal_server->port, static_cast<SoupServerListenOptions>(0), &error)) {
    g_printerr("Failed to listen for WebRTC signaling on port %u: %s\n", signal_server->port, error->message);
    g_error_free(error);
    g_object_unref(signal_server->server);
    return FALSE;
  }

  start_preview_observer(owned);
  g_mutex_lock(&webrtc_signal_servers_lock);
  webrtc_signal_servers.push_back(std::move(owned));
  g_mutex_unlock(&webrtc_signal_servers_lock);

  g_print("\n *** HStream: WebRTC preview signaling on http://0.0.0.0:%u/ ***\n\n", signal_server->port);
  return TRUE;
}

void Stop(GstElement* pipeline) {
  std::vector<std::shared_ptr<WebRtcSignalServer>> servers;
  g_mutex_lock(&webrtc_signal_servers_lock);
  for (auto it = webrtc_signal_servers.begin(); it != webrtc_signal_servers.end();) {
    if (!pipeline || gst_object_has_as_ancestor(GST_OBJECT((*it)->endpoint), GST_OBJECT(pipeline))) {
      servers.push_back(*it);
      it = webrtc_signal_servers.erase(it);
    } else {
      ++it;
    }
  }
  g_mutex_unlock(&webrtc_signal_servers_lock);

  for (const auto& state : servers) {
    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    state->stopped = true;
    if (state->frame_source) {
      g_source_destroy(state->frame_source);
      g_source_unref(state->frame_source);
      state->frame_source = nullptr;
    }
    if (state->connection) {
      auto* connection = state->connection;
      state->connection = nullptr;
      g_signal_handlers_disconnect_matched(
          connection, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr, reinterpret_cast<gpointer>(on_webrtc_ws_message), nullptr);
      g_signal_handlers_disconnect_matched(
          connection, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr, reinterpret_cast<gpointer>(on_webrtc_ws_closed), nullptr);
      soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_NORMAL, "pipeline stopping");
      g_object_unref(connection);
    }
    detach_webrtc_peer(state.get());
    if (state->server) {
      soup_server_disconnect(state->server);
      g_object_unref(state->server);
      state->server = nullptr;
    }
  }
}

} // namespace hm::webrtc
