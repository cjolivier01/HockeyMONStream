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

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <cuda_runtime_api.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include "deepstream_common.h"
#include "deepstream_sinks.h"

#include "hstream/src/apps/apps-common/HStreamBatchDemux.h"
#include "hstream/src/libs/common/VideoBitrate.h"
#include "hstream/src/libs/common/pipeline_utils.h" // For gst_element_request_pad_simple on jetson

static std::atomic<guint> next_uid = 1;
static GstRTSPServer* server[MAX_SINK_BINS];
static guint server_count = 0;
static GMutex server_cnt_lock;
static std::set<guint> rtsp_audio_sink_ids;
static std::atomic<bool> embedded_gpu_preview_video_mode{false};

void set_rtsp_audio_sink_ids(const gint* sink_ids, guint num_sink_ids) {
  rtsp_audio_sink_ids.clear();
  for (guint i = 0; i < num_sink_ids; ++i) {
    if (sink_ids[i] >= 0) {
      rtsp_audio_sink_ids.insert(static_cast<guint>(sink_ids[i]));
    }
  }
}

void set_embedded_gpu_preview_video_mode(gboolean enabled) {
  embedded_gpu_preview_video_mode = enabled != FALSE;
}

GST_DEBUG_CATEGORY_EXTERN(NVDS_APP);

namespace {

constexpr guint kWebRtcPayloadType = 96;
constexpr guint kDefaultWebRtcPort = 8080;
constexpr guint kNvEncPresetP2 = 2;

GstCaps* make_render_video_caps(const char* format, const NvDsSinkRenderConfig* config) {
  GstCaps* caps = gst_caps_new_empty_simple("video/x-raw");
  if (!caps)
    return nullptr;

  GstStructure* structure = gst_caps_get_structure(caps, 0);
  if (format && *format) {
    gst_structure_set(structure, "format", G_TYPE_STRING, format, NULL);
  }
  if (config && config->width > 0 && config->height > 0) {
    gst_structure_set(
        structure, "width", G_TYPE_INT, config->width, "height", G_TYPE_INT, config->height, NULL);
  }
  return caps;
}

struct FileEncoderBitrateScale {
  GstElement* encoder{nullptr};
  hm::BitratePerPixel bitrate_per_pixel;
  NvDsEncoderType codec{NV_DS_ENCODER_H264};
  NvDsEncHwSwType encoder_type{NV_DS_ENCODER_TYPE_HW};
  uint64_t last_bitrate_bps{0};
};

GstPadProbeReturn update_file_encoder_bitrate_from_caps(GstPad*, GstPadProbeInfo* info, gpointer user_data) {
  auto* scale = static_cast<FileEncoderBitrateScale*>(user_data);
  if (!scale || (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) == 0) {
    return GST_PAD_PROBE_OK;
  }

  GstEvent* event = gst_pad_probe_info_get_event(info);
  if (!event || GST_EVENT_TYPE(event) != GST_EVENT_CAPS) {
    return GST_PAD_PROBE_OK;
  }

  GstCaps* caps = nullptr;
  gst_event_parse_caps(event, &caps);
  const GstStructure* structure = caps && gst_caps_get_size(caps) > 0 ? gst_caps_get_structure(caps, 0) : nullptr;
  gint width = 0;
  gint height = 0;
  if (!structure || !gst_structure_get_int(structure, "width", &width) ||
      !gst_structure_get_int(structure, "height", &height) || width <= 0 || height <= 0) {
    return GST_PAD_PROBE_OK;
  }

  const std::optional<uint64_t> scaled_bitrate =
      hm::scale_bitrate(scale->bitrate_per_pixel, static_cast<uint64_t>(width), static_cast<uint64_t>(height));
  if (!scaled_bitrate.has_value() || *scaled_bitrate == 0) {
    GST_WARNING_OBJECT(scale->encoder, "Could not scale source bitrate for output caps %dx%d", width, height);
    return GST_PAD_PROBE_OK;
  }
  if (*scaled_bitrate > G_MAXINT) {
    GST_WARNING_OBJECT(
        scale->encoder,
        "Scaled bitrate %" G_GUINT64_FORMAT " exceeds the supported maximum %d; clamping",
        *scaled_bitrate,
        G_MAXINT);
  }
  const uint64_t bitrate_bps = std::min<uint64_t>(*scaled_bitrate, G_MAXINT);
  if (bitrate_bps == scale->last_bitrate_bps) {
    return GST_PAD_PROBE_OK;
  }

  if (scale->encoder_type == NV_DS_ENCODER_TYPE_SW && scale->codec != NV_DS_ENCODER_MPEG4) {
    const guint bitrate_kbps = static_cast<guint>(std::max<uint64_t>(1, (bitrate_bps + 500) / 1000));
    g_object_set(G_OBJECT(scale->encoder), "bitrate", bitrate_kbps, NULL);
  } else {
    g_object_set(G_OBJECT(scale->encoder), "bitrate", static_cast<guint>(bitrate_bps), NULL);
  }
  scale->last_bitrate_bps = bitrate_bps;
  GST_INFO_OBJECT(
      scale->encoder,
      "Set resolution-scaled file bitrate to %" G_GUINT64_FORMAT " bps for %dx%d output",
      bitrate_bps,
      width,
      height);
  g_print("HSTREAM_ARCHIVE_BITRATE bitrate=%" G_GUINT64_FORMAT " width=%d height=%d\n", bitrate_bps, width, height);
  std::fflush(stdout);
  return GST_PAD_PROBE_OK;
}

void install_file_encoder_bitrate_scaling(const NvDsSinkEncoderConfig* config, GstElement* encoder) {
  const hm::BitratePerPixel bitrate_per_pixel{
      config->bitrate_per_pixel_numerator,
      config->bitrate_per_pixel_denominator,
  };
  if (!bitrate_per_pixel.valid()) {
    return;
  }

  GstPad* sink_pad = gst_element_get_static_pad(encoder, "sink");
  if (!sink_pad) {
    GST_WARNING_OBJECT(encoder, "Could not install resolution-scaled file bitrate handling");
    return;
  }

  auto* scale = new FileEncoderBitrateScale{
      encoder,
      bitrate_per_pixel,
      config->codec,
      config->enc_type,
      0,
  };
  gst_pad_add_probe(
      sink_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, update_file_encoder_bitrate_from_caps, scale, [](gpointer data) {
        delete static_cast<FileEncoderBitrateScale*>(data);
      });
  gst_object_unref(sink_pad);
}

#ifndef IS_TEGRA
bool use_xvideo_render_sink() {
  const char* configured = std::getenv("HM_RENDER_SINK");
  return configured != nullptr &&
      (g_ascii_strcasecmp(configured, "ximagesink") == 0 || g_ascii_strcasecmp(configured, "ximage") == 0 ||
       g_ascii_strcasecmp(configured, "xvimagesink") == 0 || g_ascii_strcasecmp(configured, "xvideo") == 0 ||
       g_ascii_strcasecmp(configured, "xv") == 0);
}

bool use_nv3d_render_sink() {
  const char* configured = std::getenv("HM_RENDER_SINK");
  if (configured != nullptr && *configured != '\0') {
    std::string requested(configured);
    std::transform(requested.begin(), requested.end(), requested.begin(), [](unsigned char c) {
      return static_cast<char>(std::toupper(c));
    });
    if (requested == "NVEGLGLESSINK" || requested == "EGL" || requested == "XIMAGESINK" || requested == "XIMAGE" ||
        requested == "XVIMAGESINK" || requested == "XVIDEO" || requested == "XV") {
      return false;
    }
    if (requested != "NV3DSINK" && requested != "NV3D") {
      g_printerr(
          "Unsupported HM_RENDER_SINK=%s; using the desktop nv3dsink default "
          "(supported values: nv3dsink, nveglglessink, ximagesink)\n",
          configured);
    }
  }

  GstElementFactory* factory = gst_element_factory_find(NVDS_ELEM_SINK_3D);
  if (factory == nullptr) {
    g_printerr("nv3dsink is unavailable; falling back to nveglglessink\n");
    return false;
  }
  gst_object_unref(factory);
  return true;
}
#endif

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

    pc.addTransceiver('video', {direction: 'recvonly'});
    pc.addTransceiver('audio', {direction: 'recvonly'});
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
      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      ws.send(JSON.stringify({type: 'offer', sdp: pc.localDescription.sdp}));
    };
    ws.onmessage = async (event) => {
      const message = JSON.parse(event.data);
      if (message.type === 'answer') {
        await pc.setRemoteDescription({type: 'answer', sdp: message.sdp});
      } else if (message.type === 'candidate' && message.candidate) {
        await pc.addIceCandidate({
          candidate: message.candidate,
          sdpMLineIndex: message.sdpMLineIndex
        });
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

struct WebRtcSignalServer {
  GstElement* webrtc{nullptr};
  SoupServer* server{nullptr};
  SoupWebsocketConnection* connection{nullptr};
  GMutex lock;
  guint port{kDefaultWebRtcPort};
};

std::vector<WebRtcSignalServer*> webrtc_signal_servers;
GMutex webrtc_signal_servers_lock;

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
  SoupWebsocketConnection* connection = nullptr;
  g_mutex_lock(&signal_server->lock);
  if (signal_server->connection) {
    connection = SOUP_WEBSOCKET_CONNECTION(g_object_ref(signal_server->connection));
  }
  g_mutex_unlock(&signal_server->lock);

  if (!connection) {
    return;
  }
  soup_websocket_connection_send_text(connection, text);
  g_object_unref(connection);
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

void send_answer_message(WebRtcSignalServer* signal_server, const gchar* sdp) {
  JsonBuilder* builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "answer");
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

void on_webrtc_ice_candidate(GstElement* webrtc, guint mline_index, gchar* candidate, gpointer user_data) {
  (void)webrtc;
  send_candidate_message(static_cast<WebRtcSignalServer*>(user_data), mline_index, candidate);
}

void on_webrtc_answer_created(GstPromise* promise, gpointer user_data) {
  WebRtcSignalServer* signal_server = static_cast<WebRtcSignalServer*>(user_data);
  const GstStructure* reply = gst_promise_get_reply(promise);
  GstWebRTCSessionDescription* answer = nullptr;
  if (reply) {
    gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
  }
  gst_promise_unref(promise);

  if (!answer) {
    send_error_message(signal_server, "Failed to create WebRTC answer");
    return;
  }

  g_signal_emit_by_name(signal_server->webrtc, "set-local-description", answer, NULL);
  gchar* sdp = gst_sdp_message_as_text(answer->sdp);
  if (sdp) {
    send_answer_message(signal_server, sdp);
    g_free(sdp);
  }
  gst_webrtc_session_description_free(answer);
}

void on_webrtc_remote_description_set(GstPromise* promise, gpointer user_data) {
  WebRtcSignalServer* signal_server = static_cast<WebRtcSignalServer*>(user_data);
  gst_promise_unref(promise);
  GstPromise* answer_promise = gst_promise_new_with_change_func(on_webrtc_answer_created, signal_server, NULL);
  g_signal_emit_by_name(signal_server->webrtc, "create-answer", NULL, answer_promise);
}

void handle_webrtc_offer(WebRtcSignalServer* signal_server, const gchar* sdp_text) {
  GstSDPMessage* sdp = nullptr;
  if (gst_sdp_message_new(&sdp) != GST_SDP_OK) {
    send_error_message(signal_server, "Failed to allocate SDP message");
    return;
  }
  if (gst_sdp_message_parse_buffer(reinterpret_cast<const guint8*>(sdp_text), strlen(sdp_text), sdp) != GST_SDP_OK) {
    gst_sdp_message_free(sdp);
    send_error_message(signal_server, "Failed to parse browser SDP offer");
    return;
  }

  GstWebRTCSessionDescription* offer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);
  GstPromise* promise = gst_promise_new_with_change_func(on_webrtc_remote_description_set, signal_server, NULL);
  g_signal_emit_by_name(signal_server->webrtc, "set-remote-description", offer, promise);
  gst_webrtc_session_description_free(offer);
}

void handle_webrtc_candidate(WebRtcSignalServer* signal_server, JsonObject* object) {
  const gchar* candidate = json_string_member(object, "candidate");
  if (!candidate) {
    return;
  }
  const gint mline_index = json_int_member(object, "sdpMLineIndex", 0);
  g_signal_emit_by_name(signal_server->webrtc, "add-ice-candidate", mline_index, candidate);
}

void on_webrtc_ws_message(
    SoupWebsocketConnection* connection,
    SoupWebsocketDataType type,
    GBytes* message,
    gpointer user_data) {
  (void)connection;
  WebRtcSignalServer* signal_server = static_cast<WebRtcSignalServer*>(user_data);
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
  } else if (!g_strcmp0(message_type, "offer")) {
    const gchar* sdp = json_string_member(object, "sdp");
    if (sdp) {
      handle_webrtc_offer(signal_server, sdp);
    } else {
      send_error_message(signal_server, "Offer is missing SDP");
    }
  } else if (!g_strcmp0(message_type, "candidate")) {
    handle_webrtc_candidate(signal_server, object);
  }
  g_object_unref(parser);
}

void on_webrtc_ws_closed(SoupWebsocketConnection* connection, gpointer user_data) {
  WebRtcSignalServer* signal_server = static_cast<WebRtcSignalServer*>(user_data);
  g_mutex_lock(&signal_server->lock);
  if (signal_server->connection == connection) {
    g_object_unref(signal_server->connection);
    signal_server->connection = nullptr;
  }
  g_mutex_unlock(&signal_server->lock);
}

void on_webrtc_ws_connected(
    SoupServer* server,
    SoupWebsocketConnection* connection,
    const char* path,
    SoupClientContext* client,
    gpointer user_data) {
  (void)server;
  (void)path;
  (void)client;
  WebRtcSignalServer* signal_server = static_cast<WebRtcSignalServer*>(user_data);

  g_mutex_lock(&signal_server->lock);
  if (signal_server->connection) {
    soup_websocket_connection_close(signal_server->connection, SOUP_WEBSOCKET_CLOSE_NORMAL, "replaced");
    g_object_unref(signal_server->connection);
  }
  signal_server->connection = SOUP_WEBSOCKET_CONNECTION(g_object_ref(connection));
  g_mutex_unlock(&signal_server->lock);

  g_signal_connect(connection, "message", G_CALLBACK(on_webrtc_ws_message), signal_server);
  g_signal_connect(connection, "closed", G_CALLBACK(on_webrtc_ws_closed), signal_server);
}

void on_webrtc_http_request(
    SoupServer* server,
    SoupMessage* msg,
    const char* path,
    GHashTable* query,
    SoupClientContext* client,
    gpointer user_data) {
  (void)server;
  (void)query;
  (void)client;
  (void)user_data;
  if (g_strcmp0(path, "/") && g_strcmp0(path, "/index.html")) {
    soup_message_set_status(msg, SOUP_STATUS_NOT_FOUND);
    return;
  }
  soup_message_set_response(
      msg, "text/html; charset=utf-8", SOUP_MEMORY_STATIC, kWebRtcClientHtml, strlen(kWebRtcClientHtml));
  soup_message_set_status(msg, SOUP_STATUS_OK);
}

gboolean start_webrtc_signaling(GstElement* webrtc, const NvDsSinkEncoderConfig* config) {
  GError* error = nullptr;
  WebRtcSignalServer* signal_server = new WebRtcSignalServer();
  g_mutex_init(&signal_server->lock);
  signal_server->webrtc = GST_ELEMENT(gst_object_ref(webrtc));
  signal_server->port = config->webrtc_port ? config->webrtc_port : kDefaultWebRtcPort;
  signal_server->server = soup_server_new(SOUP_SERVER_SERVER_HEADER, "hstream-webrtc", NULL);
  if (!signal_server->server) {
    g_printerr("Failed to create WebRTC signaling server\n");
    g_mutex_clear(&signal_server->lock);
    gst_object_unref(signal_server->webrtc);
    delete signal_server;
    return FALSE;
  }

  soup_server_add_handler(signal_server->server, "/", on_webrtc_http_request, signal_server, NULL);
  soup_server_add_handler(signal_server->server, "/index.html", on_webrtc_http_request, signal_server, NULL);
  soup_server_add_websocket_handler(
      signal_server->server, "/ws", NULL, NULL, on_webrtc_ws_connected, signal_server, NULL);
  if (!soup_server_listen_all(
          signal_server->server, signal_server->port, static_cast<SoupServerListenOptions>(0), &error)) {
    g_printerr("Failed to listen for WebRTC signaling on port %u: %s\n", signal_server->port, error->message);
    g_error_free(error);
    g_object_unref(signal_server->server);
    g_mutex_clear(&signal_server->lock);
    gst_object_unref(signal_server->webrtc);
    delete signal_server;
    return FALSE;
  }

  g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(on_webrtc_ice_candidate), signal_server);

  g_mutex_lock(&webrtc_signal_servers_lock);
  webrtc_signal_servers.push_back(signal_server);
  g_mutex_unlock(&webrtc_signal_servers_lock);

  g_print("\n *** HStream: WebRTC preview signaling on http://0.0.0.0:%u/ ***\n\n", signal_server->port);
  return TRUE;
}

bool have_webrtc_ice_plugin() {
  GstElementFactory* nice_src_factory = gst_element_factory_find("nicesrc");
  GstElementFactory* nice_sink_factory = gst_element_factory_find("nicesink");
  const bool available = nice_src_factory && nice_sink_factory;
  if (nice_src_factory) {
    gst_object_unref(nice_src_factory);
  }
  if (nice_sink_factory) {
    gst_object_unref(nice_sink_factory);
  }
  return available;
}

bool rtsp_audio_enabled_for_sink_id(guint sink_id) {
  return rtsp_audio_sink_ids.count(sink_id) > 0;
}

GstPad* find_webrtc_transceiver_sink_pad(GstElement* webrtc, GstWebRTCRTPTransceiver* transceiver) {
  GstIterator* iterator = gst_element_iterate_sink_pads(webrtc);
  if (!iterator) {
    return nullptr;
  }

  GstPad* result = nullptr;
  GValue item = G_VALUE_INIT;
  bool done = false;
  while (!done) {
    switch (gst_iterator_next(iterator, &item)) {
      case GST_ITERATOR_OK: {
        GstPad* pad = GST_PAD(g_value_get_object(&item));
        GstWebRTCRTPTransceiver* pad_transceiver = nullptr;
        g_object_get(G_OBJECT(pad), "transceiver", &pad_transceiver, NULL);
        if (!gst_pad_is_linked(pad) && (!transceiver || pad_transceiver == transceiver)) {
          result = GST_PAD(gst_object_ref(pad));
          done = true;
        }
        if (pad_transceiver) {
          g_object_unref(pad_transceiver);
        }
        g_value_reset(&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync(iterator);
        break;
      case GST_ITERATOR_ERROR:
      case GST_ITERATOR_DONE:
        done = true;
        break;
    }
  }
  g_value_unset(&item);
  gst_iterator_free(iterator);
  return result;
}

void destroy_webrtc_signal_servers() {
  std::vector<WebRtcSignalServer*> servers;
  g_mutex_lock(&webrtc_signal_servers_lock);
  servers.swap(webrtc_signal_servers);
  g_mutex_unlock(&webrtc_signal_servers_lock);

  for (WebRtcSignalServer* signal_server : servers) {
    g_mutex_lock(&signal_server->lock);
    if (signal_server->connection) {
      soup_websocket_connection_close(signal_server->connection, SOUP_WEBSOCKET_CLOSE_NORMAL, "pipeline stopping");
      g_object_unref(signal_server->connection);
      signal_server->connection = nullptr;
    }
    g_mutex_unlock(&signal_server->lock);
    if (signal_server->server) {
      soup_server_disconnect(signal_server->server);
      g_object_unref(signal_server->server);
    }
    if (signal_server->webrtc) {
      gst_object_unref(signal_server->webrtc);
    }
    g_mutex_clear(&signal_server->lock);
    delete signal_server;
  }
}

} // namespace

gboolean start_webrtc_signaling_for_sink(GstElement* webrtc, const NvDsSinkEncoderConfig* config) {
  return start_webrtc_signaling(webrtc, config);
}

gboolean link_webrtc_rtp_src_to_sink(
    GstElement* webrtc,
    GstElement* rtp_src_element,
    GstCaps* rtp_caps,
    const char* track_name) {
  static std::atomic<int> webrtc_in_counter = 0;
  GstWebRTCRTPTransceiver* transceiver = nullptr;
  GstPad* webrtc_sink_pad = nullptr;
  GstPadTemplate* webrtc_sink_pad_template = nullptr;
  gchar* webrtc_sink_pad_name = nullptr;
  gboolean ret = FALSE;

  if (!webrtc || !rtp_src_element || !rtp_caps) {
    g_printerr("Cannot link WebRTC RTP track '%s': missing element or caps\n", track_name ? track_name : "unknown");
    goto done;
  }

  g_signal_emit_by_name(
      webrtc, "add-transceiver", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, rtp_caps, &transceiver);
  if (!transceiver) {
    g_printerr("Failed to add WebRTC %s transceiver\n", track_name ? track_name : "RTP");
    goto done;
  }

  webrtc_sink_pad = find_webrtc_transceiver_sink_pad(webrtc, transceiver);
  if (!webrtc_sink_pad) {
    webrtc_sink_pad_template = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(webrtc), "sink_%u");
  }
  if (!webrtc_sink_pad && webrtc_sink_pad_template) {
    webrtc_sink_pad = gst_element_request_pad(webrtc, webrtc_sink_pad_template, NULL, rtp_caps);
  }
  if (!webrtc_sink_pad) {
    webrtc_sink_pad = gst_element_request_pad_simple(webrtc, "sink_%u");
  }
  if (!webrtc_sink_pad) {
    g_printerr("Failed to request WebRTC %s RTP sink pad\n", track_name ? track_name : "RTP");
    goto done;
  }

  webrtc_sink_pad_name = gst_pad_get_name(webrtc_sink_pad);
  if (!webrtc_sink_pad_name) {
    g_printerr("Failed to name WebRTC %s RTP sink pad\n", track_name ? track_name : "RTP");
    goto done;
  }

  {
    std::string ghost_pad_name =
        std::string("webrtc_") + (track_name ? track_name : "rtp") + "_in_" + std::to_string(webrtc_in_counter++);
    ret = hm::connectElementsWithGhostPads(rtp_src_element, "src", webrtc, webrtc_sink_pad_name, ghost_pad_name);
  }

done:
  if (webrtc_sink_pad_name) {
    g_free(webrtc_sink_pad_name);
  }
  if (webrtc_sink_pad) {
    gst_object_unref(webrtc_sink_pad);
  }
  if (transceiver) {
    gst_object_unref(transceiver);
  }
  return ret;
}

namespace hm {

namespace {
std::string normalize_type_string(std::string s) {
  auto is_space = [](unsigned char c) { return std::isspace(c); };
  s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), is_space));
  s.erase(std::find_if_not(s.rbegin(), s.rend(), is_space).base(), s.end());
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
  std::replace(s.begin(), s.end(), '-', '_');
  return s;
}
} // namespace

// value to string for NvDsSinkType
std::string to_string(const NvDsSinkType& type) {
  switch (type) {
    case NV_DS_SINK_FAKE:
      return "FAKE";
#ifndef IS_TEGRA
    case NV_DS_SINK_RENDER_EGL:
      return "RENDER";
#else
    case NV_DS_SINK_RENDER_3D:
      return "RENDER";
#endif
    case NV_DS_SINK_ENCODE_FILE:
      return "ENCODE_FILE";
    case NV_DS_SINK_UDPSINK:
      return "UDPSINK";
    case NV_DS_SINK_RENDER_DRM:
      return "RENDER_DRM";
    case NV_DS_SINK_MSG_CONV_BROKER:
      return "MSG_CONV_BROKER";
    case NV_DS_SINK_WEBRTC:
      return "WEBRTC";
    case NV_DS_SINK_ENCODE_STITCHED_FILE:
      return "ENCODE_STITCHED_FILE";
    default:
      return "INVALID";
  }
}

// Converts string to enum value and returns std::optional<NvDsSinkType>
std::optional<NvDsSinkType> sink_type_from_string(const std::string& str) {
  const std::string s = normalize_type_string(str);
  if (s == "FAKE")
    return NV_DS_SINK_FAKE;
#ifndef IS_TEGRA
  if (s == "RENDER")
    return NV_DS_SINK_RENDER_EGL;
#else
  if (s == "RENDER")
    return NV_DS_SINK_RENDER_3D;
#endif
  if (s == "ENCODE_FILE" || s == "ENCODE_FIL")
    return NV_DS_SINK_ENCODE_FILE;
  if (s == "UDPSINK" || s == "RTSP" || s == "RTMP")
    return NV_DS_SINK_UDPSINK;
  if (s == "RENDER_DRM")
    return NV_DS_SINK_RENDER_DRM;
  if (s == "MSG_CONV_BROKER")
    return NV_DS_SINK_MSG_CONV_BROKER;
  if (s == "WEBRTC")
    return NV_DS_SINK_WEBRTC;
  if (s == "ENCODE_STITCHED_FILE" || s == "ARCHIVE_STITCHED")
    return NV_DS_SINK_ENCODE_STITCHED_FILE;

  // Return an empty optional if no match was found.
  return std::nullopt;
}
} // namespace hm

gboolean create_fakesink_bin(const NvDsSinkRenderConfig* config, NvDsSinkBinSubBin* bin) {
  gboolean ret = FALSE;
  gchar elem_name[50];
  GstElement* connect_to;
  GstCaps* caps = NULL;

  const guint uid = next_uid++;

  struct cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, config->gpu_id);

  g_snprintf(elem_name, sizeof(elem_name), "fakesink_sub_bin%d", uid);
  bin->bin = gst_bin_new(elem_name);
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf(elem_name, sizeof(elem_name), "fakesink_sub_bin_sink%d", uid);
  bin->sink = gst_element_factory_make(NVDS_ELEM_SINK_FAKESINK, elem_name);
  g_object_set(G_OBJECT(bin->sink), "enable-last-sample", FALSE, NULL);

  if (!bin->sink) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_object_set(G_OBJECT(bin->sink), "sync", config->sync, "max-lateness", -1, "async", FALSE, "qos", config->qos, NULL);

  if (!prop.integrated) {
    g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_cap_filter%d", uid);
    bin->cap_filter = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, elem_name);
    if (!bin->cap_filter) {
      NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
      goto done;
    }
    gst_bin_add(GST_BIN(bin->bin), bin->cap_filter);
  }

  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_transform%d", uid);

  g_snprintf(elem_name, sizeof(elem_name), "render_queue%d", uid);
  bin->queue = gst_element_factory_make(NVDS_ELEM_QUEUE, elem_name);
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  gst_bin_add_many(GST_BIN(bin->bin), bin->queue, bin->sink, NULL);

  connect_to = bin->sink;

  if (bin->cap_filter) {
    NVGSTDS_LINK_ELEMENT(bin->cap_filter, connect_to);
    connect_to = bin->cap_filter;
  }

  NVGSTDS_LINK_ELEMENT(bin->queue, connect_to);

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->queue, "sink");

  ret = TRUE;

done:
  if (caps) {
    gst_caps_unref(caps);
  }
  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }
  return ret;
}

/**
 * Function to create sink bin for Display / Fakesink.
 */
static gboolean create_render_bin(NvDsSinkRenderConfig* config, NvDsSinkBinSubBin* bin) {
  gboolean ret = FALSE;
  gchar elem_name[50];
  GstElement* connect_to;
  GstElement* system_memory_transform = nullptr;
  GstElement* system_memory_cap_filter = nullptr;
  GstCaps* caps = NULL;

  const guint uid = next_uid++;

#ifndef IS_TEGRA
  const bool use_nv3d = use_nv3d_render_sink();
  const bool use_xvideo = use_xvideo_render_sink();
#endif
  const bool gpu_preview_fake = embedded_gpu_preview_video_mode.load();
#ifdef IS_TEGRA
  const bool scaled_nv3d_render =
      !gpu_preview_fake && config->type == NV_DS_SINK_RENDER_3D && config->width > 0 && config->height > 0;
#endif

  struct cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, config->gpu_id);

  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin%d", uid);
  bin->bin = gst_bin_new(elem_name);
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_sink%d", uid);
  if (gpu_preview_fake) {
    bin->sink = gst_element_factory_make(NVDS_ELEM_SINK_FAKESINK, elem_name);
    if (bin->sink) {
      g_object_set(G_OBJECT(bin->sink), "enable-last-sample", FALSE, NULL);
    }
  } else {
    switch (config->type) {
#ifndef IS_TEGRA
      case NV_DS_SINK_RENDER_EGL:
        GST_CAT_INFO(NVDS_APP, "NVvideo renderer\n");
        bin->sink = gst_element_factory_make(
            use_nv3d ? NVDS_ELEM_SINK_3D : (use_xvideo ? "ximagesink" : NVDS_ELEM_SINK_EGL), elem_name);
        if (!use_xvideo) {
          g_object_set(
              G_OBJECT(bin->sink),
              "window-x",
              config->offset_x,
              "window-y",
              config->offset_y,
              "window-width",
              config->width,
              "window-height",
              config->height,
              NULL);
        }
        g_object_set(G_OBJECT(bin->sink), "enable-last-sample", use_xvideo ? TRUE : FALSE, NULL);
        break;
#endif
      case NV_DS_SINK_RENDER_DRM:
#ifndef IS_TEGRA
        NVGSTDS_ERR_MSG_V("nvdrmvideosink is only supported for Jetson");
        return FALSE;
#endif
        GST_CAT_INFO(NVDS_APP, "NVvideo renderer\n");
        bin->sink = gst_element_factory_make(NVDS_ELEM_SINK_DRM, elem_name);
        if ((gint)config->color_range > -1) {
          g_object_set(G_OBJECT(bin->sink), "color-range", config->color_range, NULL);
        }
        g_object_set(G_OBJECT(bin->sink), "conn-id", config->conn_id, NULL);
        g_object_set(G_OBJECT(bin->sink), "plane-id", config->plane_id, NULL);
        if ((gint)config->set_mode > -1) {
          g_object_set(G_OBJECT(bin->sink), "set-mode", config->set_mode, NULL);
        }
        break;
#ifdef IS_TEGRA
      case NV_DS_SINK_RENDER_3D:
        GST_CAT_INFO(NVDS_APP, "NVvideo renderer\n");
        bin->sink = gst_element_factory_make(NVDS_ELEM_SINK_3D, elem_name);
        g_object_set(
            G_OBJECT(bin->sink),
            "window-x",
            config->offset_x,
            "window-y",
            config->offset_y,
            "window-width",
            config->width,
            "window-height",
            config->height,
            NULL);
        g_object_set(G_OBJECT(bin->sink), "enable-last-sample", FALSE, NULL);
        break;
#endif
      case NV_DS_SINK_FAKE:
        bin->sink = gst_element_factory_make(NVDS_ELEM_SINK_FAKESINK, elem_name);
        g_object_set(G_OBJECT(bin->sink), "enable-last-sample", FALSE, NULL);
        break;
      default:
        return FALSE;
    }
  }

  if (!bin->sink) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  // Embedded/headless render-video terminators are observational and must never throttle the processing/encode
  // branch. A synchronized application-owned ximagesink can reject every late frame when inference falls behind,
  // while a synchronized fakesink needlessly clock-paces an otherwise headless render branch. Encoded and
  // self-managed render sinks retain their configured timing behavior.
#ifndef IS_TEGRA
  g_object_set(
      G_OBJECT(bin->sink),
      "sync",
      gpu_preview_fake || use_xvideo ? FALSE : config->sync,
      "max-lateness",
      -1,
      "async",
      FALSE,
      "qos",
      gpu_preview_fake || use_xvideo ? FALSE : config->qos,
      NULL);
#else
  g_object_set(
      G_OBJECT(bin->sink),
      "sync",
      gpu_preview_fake ? FALSE : config->sync,
      "max-lateness",
      -1,
      "async",
      FALSE,
      "qos",
      gpu_preview_fake ? FALSE : config->qos,
      NULL);
#endif

  if (!gpu_preview_fake &&
      (!prop.integrated
#ifndef IS_TEGRA
       || use_nv3d
#else
       || scaled_nv3d_render
#endif
       )) {
    g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_cap_filter%d", uid);
    bin->cap_filter = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, elem_name);
    if (!bin->cap_filter) {
      NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
      goto done;
    }
    gst_bin_add(GST_BIN(bin->bin), bin->cap_filter);
  }

  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_transform%d", uid);
#ifndef IS_TEGRA
  if (!gpu_preview_fake && config->type == NV_DS_SINK_RENDER_EGL) {
    if (prop.integrated && !use_nv3d) {
      bin->transform = gst_element_factory_make(NVDS_ELEM_EGLTRANSFORM, elem_name);
    } else {
      bin->transform = gst_element_factory_make(NVDS_ELEM_VIDEO_CONV, elem_name);
    }
    if (!bin->transform) {
      NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
      goto done;
    }
    gst_bin_add(GST_BIN(bin->bin), bin->transform);

    if (!prop.integrated || use_nv3d) {
      if (!use_nv3d) {
        caps = make_render_video_caps("NV12", config);
        if (!caps) {
          NVGSTDS_ERR_MSG_V("Failed to create scaled render conversion caps");
          goto done;
        }
        g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_system_transform%d", uid);
        system_memory_transform = gst_element_factory_make("videoconvert", elem_name);
        g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_system_caps%d", uid);
        system_memory_cap_filter = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, elem_name);
        if (!system_memory_transform || !system_memory_cap_filter) {
          NVGSTDS_ERR_MSG_V("Failed to create system-memory render conversion for embedded EGL output");
          goto done;
        }
        GstCaps* system_caps = make_render_video_caps("BGRx", config);
        if (!system_caps) {
          NVGSTDS_ERR_MSG_V("Failed to create scaled system-memory render caps");
          goto done;
        }
        g_object_set(G_OBJECT(system_memory_cap_filter), "caps", system_caps, NULL);
        gst_caps_unref(system_caps);
        gst_bin_add_many(GST_BIN(bin->bin), system_memory_transform, system_memory_cap_filter, NULL);
      } else {
        caps = make_render_video_caps(nullptr, config);
        if (!caps) {
          NVGSTDS_ERR_MSG_V("Failed to create scaled render caps");
          goto done;
        }
      }

      // DeepStream 9.1's nveglglessink crashes in its render thread when it receives NVMM buffers with the GStreamer
      // version shipped on this host. nvvideoconvert's nominal system-memory output still wraps an
      // NVBUF_MEM_SYSTEM surface, which X11 video sinks also cannot consume directly. For application-owned X11
      // output, first copy to pinned NV12 and then force a standard videoconvert allocation to ordinary BGRx system
      // memory. The self-managed nv3dsink path is unchanged.
      g_object_set(G_OBJECT(bin->cap_filter), "caps", caps, NULL);

      g_object_set(G_OBJECT(bin->transform), "gpu-id", config->gpu_id, NULL);
      g_object_set(G_OBJECT(bin->transform), "nvbuf-memory-type", use_nv3d ? config->nvbuf_memory_type : 1, NULL);
    }
  }
#endif
#ifdef IS_TEGRA
  if (scaled_nv3d_render) {
    bin->transform = gst_element_factory_make(NVDS_ELEM_VIDEO_CONV, elem_name);
    if (!bin->transform) {
      NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
      goto done;
    }
    gst_bin_add(GST_BIN(bin->bin), bin->transform);

    caps = make_render_video_caps(nullptr, config);
    if (!caps) {
      NVGSTDS_ERR_MSG_V("Failed to create scaled render caps");
      goto done;
    }
    g_object_set(G_OBJECT(bin->cap_filter), "caps", caps, NULL);
    g_object_set(
        G_OBJECT(bin->transform),
        "gpu-id",
        config->gpu_id,
        "nvbuf-memory-type",
        config->nvbuf_memory_type,
        NULL);
  }
#endif

  g_snprintf(elem_name, sizeof(elem_name), "render_queue%d", uid);
  bin->queue = gst_element_factory_make(NVDS_ELEM_QUEUE, elem_name);
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  gst_bin_add_many(GST_BIN(bin->bin), bin->queue, bin->sink, NULL);

  connect_to = bin->sink;

  if (system_memory_cap_filter) {
    NVGSTDS_LINK_ELEMENT(system_memory_cap_filter, connect_to);
    connect_to = system_memory_cap_filter;
  }

  if (system_memory_transform) {
    NVGSTDS_LINK_ELEMENT(system_memory_transform, connect_to);
    connect_to = system_memory_transform;
  }

  if (bin->cap_filter) {
    NVGSTDS_LINK_ELEMENT(bin->cap_filter, connect_to);
    connect_to = bin->cap_filter;
  }

  if (bin->transform) {
    NVGSTDS_LINK_ELEMENT(bin->transform, connect_to);
    connect_to = bin->transform;
  }

  NVGSTDS_LINK_ELEMENT(bin->queue, connect_to);

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->queue, "sink");

  ret = TRUE;

done:
  if (caps) {
    gst_caps_unref(caps);
  }
  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }
  return ret;
}

static void broker_queue_overrun(GstElement* sink_queue, gpointer user_data) {
  (void)sink_queue;
  (void)user_data;
  NVGSTDS_WARN_MSG_V(
      "nvmsgbroker queue overrun; Older Message Buffer "
      "Dropped; Network bandwidth might be insufficient\n");
}

/**
 * Function to create sink bin to generate meta-msg, convert to json based on
 * a schema and send over msgbroker.
 */
static gboolean create_msg_conv_broker_bin(NvDsSinkMsgConvBrokerConfig* config, NvDsSinkBinSubBin* bin) {
  /** Create the subbin: -> q -> msgconv -> msgbroker bin */
  gboolean ret = FALSE;
  gchar elem_name[50];

  const guint uid = next_uid++;

  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin%d", uid);
  bin->bin = gst_bin_new(elem_name);
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }
  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_queue%d", uid);
  bin->queue = gst_element_factory_make(NVDS_ELEM_QUEUE, elem_name);
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  /** set threshold on queue to avoid pipeline choke when broker is stuck on network
   * leaky=2 (2): downstream       - Leaky on downstream (old buffers) */
  g_object_set(G_OBJECT(bin->queue), "leaky", 2, NULL);
  g_object_set(G_OBJECT(bin->queue), "max-size-buffers", 20, NULL);
  g_signal_connect(G_OBJECT(bin->queue), "overrun", G_CALLBACK(broker_queue_overrun), bin);

  /* Create msg converter to generate payload from buffer metadata */
  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_transform%d", uid);
  if (config->disable_msgconv) {
    bin->transform = gst_element_factory_make("queue", elem_name);
  } else {
    bin->transform = gst_element_factory_make(NVDS_ELEM_MSG_CONV, elem_name);
  }
  if (!bin->transform) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  if (!config->disable_msgconv)
    g_object_set(
        G_OBJECT(bin->transform),
        "config",
        config->config_file_path,
        "msg2p-lib",
        (config->conv_msg2p_lib ? config->conv_msg2p_lib : NULL),
        "payload-type",
        config->conv_payload_type,
        "comp-id",
        config->conv_comp_id,
        "debug-payload-dir",
        config->debug_payload_dir,
        "multiple-payloads",
        config->multiple_payloads,
        "msg2p-newapi",
        config->conv_msg2p_new_api,
        "frame-interval",
        config->conv_frame_interval,
        "dummy-payload",
        config->conv_dummy_payload,
        NULL);

  /* Create msg broker to send payload to server */
  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_sink%d", uid);
  bin->sink = gst_element_factory_make(NVDS_ELEM_MSG_BROKER, elem_name);
  if (!bin->sink) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }
  g_object_set(
      G_OBJECT(bin->sink),
      "proto-lib",
      config->proto_lib,
      "conn-str",
      config->conn_str,
      "topic",
      config->topic,
      "sync",
      config->sync,
      "async",
      FALSE,
      "config",
      config->broker_config_file_path,
      "comp-id",
      config->broker_comp_id,
      "new-api",
      config->new_api,
      "sleep-time",
      config->broker_sleep_time,
      NULL);

  gst_bin_add_many(GST_BIN(bin->bin), bin->queue, bin->transform, bin->sink, NULL);

  NVGSTDS_LINK_ELEMENT(bin->queue, bin->transform);
  NVGSTDS_LINK_ELEMENT(bin->transform, bin->sink);

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->queue, "sink");

  ret = TRUE;

done:
  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }
  return ret;
}

/**
 * Probe function to drop upstream "GST_QUERY_SEEKING" query from h264parse element.
 * This is a WAR to avoid memory leaks from h264parse element
 */
static GstPadProbeReturn seek_query_drop_prob(GstPad* pad, GstPadProbeInfo* info, gpointer u_data) {
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_QUERY_UPSTREAM) {
    GstQuery* query = GST_PAD_PROBE_INFO_QUERY(info);
    if (GST_QUERY_TYPE(query) == GST_QUERY_SEEKING) {
      return GST_PAD_PROBE_DROP;
    }
  }
  return GST_PAD_PROBE_OK;
}

bool link_video_pad_to_muxer(GstElement* postParse, GstElement* muxer) {
  GstPad* postParse_src = gst_element_get_static_pad(postParse, "src");
  if (!postParse_src) {
    g_printerr("Could not get postParse src pad.\n");
    return false;
  }
  GstPad* muxer_video_pad = gst_element_request_pad_simple(muxer, "video_%u");
  if (!muxer_video_pad) {
    g_printerr("Could not get request pad from muxer for video.\n");
    return false;
  }
  if (gst_pad_link(postParse_src, muxer_video_pad) != GST_PAD_LINK_OK) {
    g_printerr("Failed to link postParse to muxer (video branch).\n");
    return false;
  }
  gst_object_unref(postParse_src);
  gst_object_unref(muxer_video_pad);
  return true;
}

/**
 * Function to create sink bin to generate encoded output.
 */
static gboolean create_encode_file_bin(
    NvDsSinkEncoderConfig* config,
    NvDsSinkBinSubBin* bin,
    gboolean stitched_output = FALSE,
    gboolean main10_output = FALSE) {
  GstCaps* caps = NULL;
  gboolean ret = FALSE;
  gchar elem_name[50];
  int probe_id = 0;
  gulong bitrate = config->bitrate;
  guint profile = config->profile;
  const gchar* latency = g_getenv("NVDS_ENABLE_LATENCY_MEASUREMENT");

  const guint uid = next_uid++;

  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin%d", uid);
  bin->bin = gst_bin_new(elem_name);
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_queue%d", uid);
  bin->queue = gst_element_factory_make(NVDS_ELEM_QUEUE, elem_name);
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_transform%d", uid);
  bin->transform = gst_element_factory_make(NVDS_ELEM_VIDEO_CONV, elem_name);
  if (!bin->transform) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }
  g_object_set(G_OBJECT(bin->transform), "compute-hw", config->compute_hw, NULL);

#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  /* For Jetson, with copy-hw=1 and memory-type=nvbuf-mem-surface-array,
     cudaMemcopy fail is observed. This is a WAR till root cause is fixed */
  g_object_set(G_OBJECT(bin->transform), "copy-hw", 2, NULL);
#endif

  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_cap_filter%d", uid);
  bin->cap_filter = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, elem_name);
  if (!bin->cap_filter) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_encoder%d", uid);
  switch (config->codec) {
    case NV_DS_ENCODER_H264:
      if (config->enc_type == NV_DS_ENCODER_TYPE_SW) {
        bin->encoder = gst_element_factory_make(NVDS_ELEM_ENC_H264_SW, elem_name);
      } else {
        bin->encoder = gst_element_factory_make(NVDS_ELEM_ENC_H264_HW, elem_name);
        if (!bin->encoder) {
          NVGSTDS_INFO_MSG_V("Could not create HW encoder. Falling back to SW encoder");
          bin->encoder = gst_element_factory_make(NVDS_ELEM_ENC_H264_SW, elem_name);
          config->enc_type = NV_DS_ENCODER_TYPE_SW;
        }
      }
      break;
    case NV_DS_ENCODER_H265:
      if (config->enc_type == NV_DS_ENCODER_TYPE_SW) {
        bin->encoder = gst_element_factory_make(NVDS_ELEM_ENC_H265_SW, elem_name);
      } else {
        bin->encoder = gst_element_factory_make(NVDS_ELEM_ENC_H265_HW, elem_name);
        if (!bin->encoder) {
          NVGSTDS_INFO_MSG_V("Could not create HW encoder. Falling back to SW encoder");
          bin->encoder = gst_element_factory_make(NVDS_ELEM_ENC_H265_SW, elem_name);
          config->enc_type = NV_DS_ENCODER_TYPE_SW;
        }
      }
      break;
    case NV_DS_ENCODER_MPEG4:
      bin->encoder = gst_element_factory_make(NVDS_ELEM_ENC_MPEG4, elem_name);
      break;
    default:
      goto done;
  }
  if (!bin->encoder) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  if (main10_output && (config->codec != NV_DS_ENCODER_H265 || config->enc_type != NV_DS_ENCODER_TYPE_HW)) {
    NVGSTDS_ERR_MSG_V("Main10 stitched archives require the hardware HEVC encoder");
    goto done;
  }
  if (main10_output)
    caps = gst_caps_from_string("video/x-raw(memory:NVMM), format=P010_10LE, colorimetry=bt709");
  else if (config->codec == NV_DS_ENCODER_MPEG4 || config->enc_type == NV_DS_ENCODER_TYPE_SW)
    caps = gst_caps_from_string("video/x-raw, format=I420");
  else
    caps = gst_caps_from_string("video/x-raw(memory:NVMM), format=I420");
  g_object_set(G_OBJECT(bin->cap_filter), "caps", caps, NULL);

  if (stitched_output) {
    if (!register_hstream_batch_demux()) {
      NVGSTDS_ERR_MSG_V("Failed to register the stitched archive batch demuxer");
      goto done;
    }
    g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_batch_demux%d", uid);
    bin->batch_demux = gst_element_factory_make("hstreambatchdemux", elem_name);
    if (!bin->batch_demux) {
      NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
      goto done;
    }
  }

  NVGSTDS_ELEM_ADD_PROBE(probe_id, bin->encoder, "sink", seek_query_drop_prob, GST_PAD_PROBE_TYPE_QUERY_UPSTREAM, bin);

  probe_id = probe_id;

  if (config->codec == NV_DS_ENCODER_MPEG4)
    config->enc_type = NV_DS_ENCODER_TYPE_SW;

  struct cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, config->gpu_id);

  if (config->copy_meta == 1) {
    g_object_set(G_OBJECT(bin->encoder), "copy-meta", TRUE, NULL);
  }

  if (config->enc_type == NV_DS_ENCODER_TYPE_HW) {
    switch (config->output_io_mode) {
      case NV_DS_ENCODER_OUTPUT_IO_MODE_MMAP:
      default:
        g_object_set(G_OBJECT(bin->encoder), "output-io-mode", NV_DS_ENCODER_OUTPUT_IO_MODE_MMAP, NULL);
        break;
      case NV_DS_ENCODER_OUTPUT_IO_MODE_DMABUF_IMPORT:
        g_object_set(G_OBJECT(bin->encoder), "output-io-mode", NV_DS_ENCODER_OUTPUT_IO_MODE_DMABUF_IMPORT, NULL);
        break;
    }
  }

  if (config->enc_type == NV_DS_ENCODER_TYPE_HW) {
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(bin->encoder), "preset-id")) {
      g_object_set(G_OBJECT(bin->encoder), "preset-id", kNvEncPresetP2, NULL);
    } else {
      GST_WARNING_OBJECT(bin->encoder, "Hardware file encoder does not expose preset-id; cannot select NVENC P2");
    }
    g_object_set(G_OBJECT(bin->encoder), "profile", profile, NULL);
    g_object_set(G_OBJECT(bin->encoder), "iframeinterval", config->iframeinterval, NULL);
    g_object_set(G_OBJECT(bin->encoder), "bitrate", bitrate, NULL);
    g_object_set(G_OBJECT(bin->encoder), "gpu-id", config->gpu_id, NULL);
    if (main10_output && g_object_class_find_property(G_OBJECT_GET_CLASS(bin->encoder), "insert-vui")) {
      g_object_set(G_OBJECT(bin->encoder), "insert-vui", TRUE, NULL);
    }
  } else {
    if (config->codec == NV_DS_ENCODER_MPEG4)
      g_object_set(G_OBJECT(bin->encoder), "bitrate", bitrate, NULL);
    else {
      // bitrate is in kbits/sec for software encoder x264enc and x265enc
      g_object_set(G_OBJECT(bin->encoder), "bitrate", bitrate / 1000, NULL);
      g_object_set(G_OBJECT(bin->encoder), "speed-preset", config->sw_preset, NULL);
    }
  }
  install_file_encoder_bitrate_scaling(config, bin->encoder);

  switch (config->codec) {
    case NV_DS_ENCODER_H264:
      bin->codecparse = gst_element_factory_make("h264parse", "h264-parser");
      break;
    case NV_DS_ENCODER_H265:
      bin->codecparse = gst_element_factory_make("h265parse", "h265-parser");
      break;
    case NV_DS_ENCODER_MPEG4:
      bin->codecparse = gst_element_factory_make("mpeg4videoparse", "mpeg4-parser");
      break;
    default:
      goto done;
  }

  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_mux%d", uid);
  // disabling the mux when latency measurement logs are enabled
  if (latency) {
    bin->mux = gst_element_factory_make(NVDS_ELEM_IDENTITY, elem_name);
  } else {
    switch (config->container) {
      case NV_DS_CONTAINER_MP4:
        bin->mux = gst_element_factory_make(NVDS_ELEM_MUX_MP4, elem_name);
        break;
      case NV_DS_CONTAINER_MKV:
        bin->mux = gst_element_factory_make(NVDS_ELEM_MKV, elem_name);
        break;
      default:
        goto done;
    }
  }

  if (!bin->mux) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_sink%d", uid);
  bin->sink = gst_element_factory_make(NVDS_ELEM_SINK_FILE, elem_name);
  if (!bin->sink) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_object_set(G_OBJECT(bin->sink), "location", config->output_file_path, "sync", config->sync, "async", FALSE, NULL);
  g_object_set(G_OBJECT(bin->transform), "gpu-id", config->gpu_id, NULL);
  gst_bin_add_many(
      GST_BIN(bin->bin),
      bin->queue,
      bin->transform,
      bin->codecparse,
      bin->cap_filter,
      bin->encoder,
      bin->mux,
      bin->sink,
      NULL);
  if (bin->batch_demux) {
    gst_bin_add(GST_BIN(bin->bin), bin->batch_demux);
  }

  NVGSTDS_LINK_ELEMENT(bin->queue, bin->transform);

  NVGSTDS_LINK_ELEMENT(bin->transform, bin->cap_filter);
  if (bin->batch_demux) {
    NVGSTDS_LINK_ELEMENT(bin->cap_filter, bin->batch_demux);
    GstPad* demux_src_pad = gst_element_request_pad_simple(bin->batch_demux, "src_0");
    GstPad* encoder_sink_pad = gst_element_get_static_pad(bin->encoder, "sink");
    if (!demux_src_pad || !encoder_sink_pad || gst_pad_link(demux_src_pad, encoder_sink_pad) != GST_PAD_LINK_OK) {
      if (demux_src_pad)
        gst_object_unref(demux_src_pad);
      if (encoder_sink_pad)
        gst_object_unref(encoder_sink_pad);
      NVGSTDS_ERR_MSG_V("Failed to link the stitched archive batch demuxer to the encoder");
      goto done;
    }
    gst_object_unref(demux_src_pad);
    gst_object_unref(encoder_sink_pad);
  } else {
    NVGSTDS_LINK_ELEMENT(bin->cap_filter, bin->encoder);
  }

  NVGSTDS_LINK_ELEMENT(bin->encoder, bin->codecparse);
  // NVGSTDS_LINK_ELEMENT(bin->codecparse, bin->mux);

  if (!link_video_pad_to_muxer(bin->codecparse, bin->mux)) {
    goto done;
  }

  NVGSTDS_LINK_ELEMENT(bin->mux, bin->sink);

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->queue, "sink");

  ret = TRUE;

done:
  if (caps) {
    gst_caps_unref(caps);
  }
  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }
  return ret;
}

static gboolean start_rtsp_streaming(
    guint rtsp_port_num,
    guint video_udpsink_port_num,
    guint audio_udpsink_port_num,
    gboolean enable_audio,
    NvDsEncoderType enctype,
    guint64 udp_buffer_size) {
  GstRTSPMountPoints* mounts;
  GstRTSPMediaFactory* factory;
  char udpsrc_pipeline[2048];

  char port_num_Str[64] = {0};
  const char* encoder_name;
  const char* depay_name;
  const char* parse_name;
  const char* pay_name;

  if (enctype == NV_DS_ENCODER_H264) {
    encoder_name = "H264";
    depay_name = "rtph264depay";
    parse_name = "h264parse";
    pay_name = "rtph264pay";
  } else if (enctype == NV_DS_ENCODER_H265) {
    encoder_name = "H265";
    depay_name = "rtph265depay";
    parse_name = "h265parse";
    pay_name = "rtph265pay";
  } else {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
    return FALSE;
  }

  if (udp_buffer_size == 0)
    udp_buffer_size = 512 * 1024;

  if (enable_audio) {
    g_snprintf(
        udpsrc_pipeline,
        sizeof(udpsrc_pipeline),
        "( udpsrc port=%d buffer-size=%lu caps=\"application/x-rtp, media=(string)video, "
        "clock-rate=(int)90000, encoding-name=(string)%s, payload=(int)96\" ! "
        "%s ! %s config-interval=1 ! %s name=pay0 pt=96 config-interval=1 )"
        " ( udpsrc port=%d buffer-size=%lu caps=\"application/x-rtp, media=(string)audio, "
        "clock-rate=(int)%u, encoding-name=(string)L16, payload=(int)%u, channels=(int)%u, "
        "encoding-params=(string)%u\" ! "
        "rtpL16depay ! audio/x-raw, format=(string)S16BE, layout=(string)interleaved, "
        "rate=(int)%u, channels=(int)%u ! rtpL16pay name=pay1 pt=%u )",
        video_udpsink_port_num,
        udp_buffer_size,
        encoder_name,
        depay_name,
        parse_name,
        pay_name,
        audio_udpsink_port_num,
        udp_buffer_size,
        hm::kRtspAudioRate,
        hm::kRtspAudioPayloadType,
        hm::kRtspAudioChannels,
        hm::kRtspAudioChannels,
        hm::kRtspAudioRate,
        hm::kRtspAudioChannels,
        hm::kRtspAudioPayloadType);
  } else {
    g_snprintf(
        udpsrc_pipeline,
        sizeof(udpsrc_pipeline),
        "( udpsrc port=%d buffer-size=%lu caps=\"application/x-rtp, media=(string)video, "
        "clock-rate=(int)90000, encoding-name=(string)%s, payload=(int)96\" ! "
        "%s ! %s config-interval=1 ! %s name=pay0 pt=96 config-interval=1 )",
        video_udpsink_port_num,
        udp_buffer_size,
        encoder_name,
        depay_name,
        parse_name,
        pay_name);
  }

  sprintf(port_num_Str, "%d", rtsp_port_num);

  g_mutex_lock(&server_cnt_lock);

  server[server_count] = gst_rtsp_server_new();
  g_object_set(server[server_count], "address", "0.0.0.0", NULL);
  g_object_set(server[server_count], "service", port_num_Str, NULL);

  mounts = gst_rtsp_server_get_mount_points(server[server_count]);

  factory = gst_rtsp_media_factory_new();
  gst_rtsp_media_factory_set_shared(factory, TRUE);
  gst_rtsp_media_factory_set_transport_mode(factory, GST_RTSP_TRANSPORT_MODE_PLAY);
  gst_rtsp_media_factory_set_protocols(factory, GST_RTSP_LOWER_TRANS_TCP);
  gst_rtsp_media_factory_set_launch(factory, udpsrc_pipeline);

  gst_rtsp_mount_points_add_factory(mounts, "/ds-test", factory);

  g_object_unref(mounts);

  gst_rtsp_server_attach(server[server_count], NULL);

  server_count++;

  g_mutex_unlock(&server_cnt_lock);

  if (enable_audio) {
    g_print(
        "\n *** DeepStream: Launched RTSP Streaming on 0.0.0.0:%d at /ds-test "
        "(video UDP %u, audio UDP %u) ***\n\n",
        rtsp_port_num,
        video_udpsink_port_num,
        audio_udpsink_port_num);
  } else {
    g_print(
        "\n *** DeepStream: Launched RTSP Streaming on 0.0.0.0:%d at /ds-test "
        "(video UDP %u) ***\n\n",
        rtsp_port_num,
        video_udpsink_port_num);
  }

  return TRUE;
}

enum ServerSinkType { SST_RTSP, SST_RTMP };

static enum ServerSinkType get_server_sink_type(const char* s) {
  if (!strncmp(s, "rtmp:/", 6)) {
    return SST_RTMP;
  }
  return SST_RTSP;
}

static gboolean create_udpsink_bin(NvDsSinkEncoderConfig* config, NvDsSinkBinSubBin* bin, gboolean enable_rtsp_audio) {
  GstCaps* caps = NULL;
  gboolean ret = FALSE;
  gchar elem_name[50];
  gchar encode_name[50];
  gchar rtppay_or_flvmux_name[50];
  int probe_id = 0;
  enum ServerSinkType sink_type;
  bool resize_rtsp_output = false;
  std::string caps_string;
  const guint iframe_interval = config->iframeinterval ? config->iframeinterval : 30;

  // guint rtsp_port_num = g_rtsp_port_num++;
  const guint uid = next_uid++;

  sink_type = get_server_sink_type(config->output_file_path);
  if (sink_type == SST_RTMP) {
    config->codec = NV_DS_ENCODER_H264;
  }

  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin%d", uid);
  bin->bin = gst_bin_new(elem_name);
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_queue%d", uid);
  bin->queue = gst_element_factory_make(NVDS_ELEM_QUEUE, elem_name);
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_transform%d", uid);
  bin->transform = gst_element_factory_make(NVDS_ELEM_VIDEO_CONV, elem_name);
  if (!bin->transform) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }
  g_object_set(G_OBJECT(bin->transform), "compute-hw", config->compute_hw, NULL);

#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  /* For Jetson, with copy-hw=1 and memory-type=nvbuf-mem-surface-array,
     cudaMemcopy fail is observed. This is a WAR till root cause is fixed */
  g_object_set(G_OBJECT(bin->transform), "copy-hw", 2, NULL);
#endif

  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_cap_filter%d", uid);
  bin->cap_filter = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, elem_name);
  if (!bin->cap_filter) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf(encode_name, sizeof(encode_name), "sink_sub_bin_encoder%d", uid);

  switch (config->codec) {
    case NV_DS_ENCODER_H264:
      bin->codecparse = gst_element_factory_make("h264parse", "h264-parser");
      g_object_set(G_OBJECT(bin->codecparse), "config-interval", sink_type == SST_RTMP ? -1 : 1, NULL);
      if (sink_type != SST_RTMP) {
        g_snprintf(rtppay_or_flvmux_name, sizeof(rtppay_or_flvmux_name), "sink_sub_bin_rtppay_or_flvmux%d", uid);
        bin->rtppay_or_flvmux = gst_element_factory_make("rtph264pay", rtppay_or_flvmux_name);
      } else {
        g_snprintf(rtppay_or_flvmux_name, sizeof(rtppay_or_flvmux_name), "sink_sub_bin_flvmux%d", uid);
        bin->rtppay_or_flvmux = gst_element_factory_make("flvmux", rtppay_or_flvmux_name);
      }
      if (config->enc_type == NV_DS_ENCODER_TYPE_SW) {
        bin->encoder = gst_element_factory_make(NVDS_ELEM_ENC_H264_SW, encode_name);
      } else {
        bin->encoder = gst_element_factory_make(NVDS_ELEM_ENC_H264_HW, encode_name);
        if (!bin->encoder) {
          NVGSTDS_INFO_MSG_V("Could not create HW encoder. Falling back to SW encoder");
          bin->encoder = gst_element_factory_make(NVDS_ELEM_ENC_H264_SW, encode_name);
          config->enc_type = NV_DS_ENCODER_TYPE_SW;
        }
      }
      break;
    case NV_DS_ENCODER_H265:
      bin->codecparse = gst_element_factory_make("h265parse", "h265-parser");
      g_object_set(G_OBJECT(bin->codecparse), "config-interval", 1, NULL);
      g_snprintf(rtppay_or_flvmux_name, sizeof(rtppay_or_flvmux_name), "sink_sub_bin_rtppay_or_flvmux%d", uid);
      bin->rtppay_or_flvmux = gst_element_factory_make("rtph265pay", rtppay_or_flvmux_name);
      if (config->enc_type == NV_DS_ENCODER_TYPE_SW) {
        bin->encoder = gst_element_factory_make(NVDS_ELEM_ENC_H265_SW, encode_name);
      } else {
        bin->encoder = gst_element_factory_make(NVDS_ELEM_ENC_H265_HW, encode_name);
        if (!bin->encoder) {
          NVGSTDS_INFO_MSG_V("Could not create HW encoder. Falling back to SW encoder");
          bin->encoder = gst_element_factory_make(NVDS_ELEM_ENC_H265_SW, encode_name);
          config->enc_type = NV_DS_ENCODER_TYPE_SW;
        }
      }
      break;
    default:
      goto done;
  }

  if (!bin->encoder) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", encode_name);
    goto done;
  }

  resize_rtsp_output = sink_type != SST_RTMP && config->width && config->height;
  caps_string =
      config->enc_type == NV_DS_ENCODER_TYPE_SW ? "video/x-raw, format=I420" : "video/x-raw(memory:NVMM), format=I420";
  if (resize_rtsp_output) {
    caps_string += ", width=(int)" + std::to_string(config->width) + ", height=(int)" + std::to_string(config->height);
  }
  caps = gst_caps_from_string(caps_string.c_str());

  g_object_set(G_OBJECT(bin->cap_filter), "caps", caps, NULL);

  NVGSTDS_ELEM_ADD_PROBE(probe_id, bin->encoder, "sink", seek_query_drop_prob, GST_PAD_PROBE_TYPE_QUERY_UPSTREAM, bin);

  probe_id = probe_id;

  if (!bin->rtppay_or_flvmux) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", rtppay_or_flvmux_name);
    goto done;
  }
  if (sink_type != SST_RTMP) {
    g_object_set(G_OBJECT(bin->rtppay_or_flvmux), "pt", 96, "config-interval", 1, NULL);
  }

  if (config->enc_type == NV_DS_ENCODER_TYPE_SW) {
    // bitrate is in kbits/sec for software encoder x264enc and x265enc
    g_object_set(G_OBJECT(bin->encoder), "bitrate", config->bitrate / 1000, NULL);
    if (sink_type != SST_RTMP && g_object_class_find_property(G_OBJECT_GET_CLASS(bin->encoder), "key-int-max")) {
      g_object_set(G_OBJECT(bin->encoder), "key-int-max", iframe_interval, NULL);
    }
  } else {
    g_object_set(G_OBJECT(bin->encoder), "bitrate", config->bitrate, NULL);
    g_object_set(G_OBJECT(bin->encoder), "profile", config->profile, NULL);
    g_object_set(G_OBJECT(bin->encoder), "iframeinterval", iframe_interval, NULL);
    if (sink_type != SST_RTMP) {
      if (g_object_class_find_property(G_OBJECT_GET_CLASS(bin->encoder), "idrinterval")) {
        g_object_set(G_OBJECT(bin->encoder), "idrinterval", iframe_interval, NULL);
      }
      if (g_object_class_find_property(G_OBJECT_GET_CLASS(bin->encoder), "insert-sps-pps")) {
        g_object_set(G_OBJECT(bin->encoder), "insert-sps-pps", 1, NULL);
      }
    }
  }

  struct cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, config->gpu_id);

  if (prop.integrated) {
    if (config->enc_type == NV_DS_ENCODER_TYPE_HW) {
      g_object_set(G_OBJECT(bin->encoder), "preset-level", 1, NULL);
      g_object_set(G_OBJECT(bin->encoder), "insert-sps-pps", 1, NULL);
      g_object_set(G_OBJECT(bin->encoder), "gpu-id", config->gpu_id, NULL);
    }
  } else {
    g_object_set(G_OBJECT(bin->transform), "gpu-id", config->gpu_id, NULL);
  }

  g_snprintf(elem_name, sizeof(elem_name), "sink_sub_bin_udpsink%d", uid);

  if (sink_type != SST_RTMP) {
    bin->sink = gst_element_factory_make("udpsink", elem_name);
  } else {
    bin->sink = gst_element_factory_make("rtmpsink", elem_name);
  }
  if (!bin->sink) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  if (sink_type != SST_RTMP) {
    g_object_set(
        G_OBJECT(bin->sink), "host", "127.0.0.1", "port", config->udp_port, "async", FALSE, "sync", config->sync, NULL);
  } else {
    std::cout << "Setting up to stream to RTMP server at " << config->output_file_path << std::endl;
    g_object_set(G_OBJECT(bin->sink), "location", config->output_file_path, "async", FALSE, "sync", config->sync, NULL);
    g_object_set(G_OBJECT(bin->rtppay_or_flvmux), "streamable", TRUE, NULL);
  }

  gst_bin_add_many(
      GST_BIN(bin->bin),
      bin->queue,
      bin->cap_filter,
      bin->transform,
      bin->encoder,
      bin->codecparse,
      bin->rtppay_or_flvmux,
      bin->sink,
      NULL);

  NVGSTDS_LINK_ELEMENT(bin->queue, bin->transform);
  NVGSTDS_LINK_ELEMENT(bin->transform, bin->cap_filter);
  NVGSTDS_LINK_ELEMENT(bin->cap_filter, bin->encoder);
  NVGSTDS_LINK_ELEMENT(bin->encoder, bin->codecparse);
  if (sink_type != SST_RTMP) {
    NVGSTDS_LINK_ELEMENT(bin->codecparse, bin->rtppay_or_flvmux);
  } else {
    if (!gst_element_link_pads(bin->codecparse, "src", bin->rtppay_or_flvmux, "video")) {
      g_printerr("Failed to link h264parse to flvmux\n");
      goto done;
    }
  }
  NVGSTDS_LINK_ELEMENT(bin->rtppay_or_flvmux, bin->sink);

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->queue, "sink");

  ret = TRUE;

  if (sink_type != SST_RTMP) {
    ret = start_rtsp_streaming(
        config->rtsp_port,
        config->udp_port,
        config->udp_port + hm::kRtspAudioUdpPortOffset,
        enable_rtsp_audio,
        config->codec,
        config->udp_buffer_size);
    if (ret != TRUE) {
      g_print("%s: start_rtsp_straming function failed\n", __func__);
    }
  }
done:
  if (caps) {
    gst_caps_unref(caps);
  }
  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }
  return ret;
}

static gboolean create_webrtc_sink_bin(NvDsSinkEncoderConfig* config, NvDsSinkBinSubBin* bin) {
  GstCaps* raw_caps = NULL;
  GstCaps* rtp_caps = NULL;
  GstWebRTCRTPTransceiver* transceiver = NULL;
  GstPad* rtp_src_pad = NULL;
  GstPad* webrtc_sink_pad = NULL;
  GstPadTemplate* webrtc_sink_pad_template = NULL;
  gboolean ret = FALSE;
  gchar elem_name[64];
  gchar encode_name[64];
  gchar rtppay_name[64];
  struct cudaDeviceProp prop;
  bool resize_output = false;
  std::string caps_string;
  const guint iframe_interval = config->iframeinterval ? config->iframeinterval : 30;
  GstPadLinkReturn link_result = GST_PAD_LINK_REFUSED;

  const guint uid = next_uid++;
  config->codec = NV_DS_ENCODER_H264;

  if (!have_webrtc_ice_plugin()) {
    g_printerr(
        "WEBRTC sink requires the GStreamer libnice plugin (nicesrc/nicesink). "
        "Install the gstreamer1.0-nice package.\n");
    goto done;
  }

  g_snprintf(elem_name, sizeof(elem_name), "webrtc_sink_sub_bin%d", uid);
  bin->bin = gst_bin_new(elem_name);
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf(elem_name, sizeof(elem_name), "webrtc_sink_sub_bin_queue%d", uid);
  bin->queue = gst_element_factory_make(NVDS_ELEM_QUEUE, elem_name);
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf(elem_name, sizeof(elem_name), "webrtc_sink_sub_bin_transform%d", uid);
  bin->transform = gst_element_factory_make(NVDS_ELEM_VIDEO_CONV, elem_name);
  if (!bin->transform) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }
  g_object_set(G_OBJECT(bin->transform), "compute-hw", config->compute_hw, NULL);

#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  g_object_set(G_OBJECT(bin->transform), "copy-hw", 2, NULL);
#endif

  g_snprintf(elem_name, sizeof(elem_name), "webrtc_sink_sub_bin_cap_filter%d", uid);
  bin->cap_filter = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, elem_name);
  if (!bin->cap_filter) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf(encode_name, sizeof(encode_name), "webrtc_sink_sub_bin_encoder%d", uid);
  if (config->enc_type == NV_DS_ENCODER_TYPE_SW) {
    bin->encoder = gst_element_factory_make(NVDS_ELEM_ENC_H264_SW, encode_name);
  } else {
    bin->encoder = gst_element_factory_make(NVDS_ELEM_ENC_H264_HW, encode_name);
    if (!bin->encoder) {
      NVGSTDS_INFO_MSG_V("Could not create HW encoder. Falling back to SW encoder");
      bin->encoder = gst_element_factory_make(NVDS_ELEM_ENC_H264_SW, encode_name);
      config->enc_type = NV_DS_ENCODER_TYPE_SW;
    }
  }
  if (!bin->encoder) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", encode_name);
    goto done;
  }

  bin->codecparse = gst_element_factory_make("h264parse", "webrtc-h264-parser");
  if (!bin->codecparse) {
    NVGSTDS_ERR_MSG_V("Failed to create 'webrtc-h264-parser'");
    goto done;
  }
  g_object_set(G_OBJECT(bin->codecparse), "config-interval", 1, NULL);

  g_snprintf(rtppay_name, sizeof(rtppay_name), "webrtc_sink_sub_bin_rtph264pay%d", uid);
  bin->rtppay_or_flvmux = gst_element_factory_make("rtph264pay", rtppay_name);
  if (!bin->rtppay_or_flvmux) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", rtppay_name);
    goto done;
  }
  g_object_set(G_OBJECT(bin->rtppay_or_flvmux), "pt", kWebRtcPayloadType, "config-interval", 1, NULL);
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(bin->rtppay_or_flvmux), "aggregate-mode")) {
    g_object_set(G_OBJECT(bin->rtppay_or_flvmux), "aggregate-mode", 1, NULL);
  }

  g_snprintf(elem_name, sizeof(elem_name), "webrtc_sink_sub_bin_rtp_caps%d", uid);
  bin->enc_caps_filter = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, elem_name);
  if (!bin->enc_caps_filter) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }
  rtp_caps = gst_caps_new_simple(
      "application/x-rtp",
      "media",
      G_TYPE_STRING,
      "video",
      "encoding-name",
      G_TYPE_STRING,
      "H264",
      "payload",
      G_TYPE_INT,
      kWebRtcPayloadType,
      "clock-rate",
      G_TYPE_INT,
      90000,
      "packetization-mode",
      G_TYPE_STRING,
      "1",
      "profile-level-id",
      G_TYPE_STRING,
      "42e01f",
      NULL);
  g_object_set(G_OBJECT(bin->enc_caps_filter), "caps", rtp_caps, NULL);

  g_snprintf(elem_name, sizeof(elem_name), "webrtc_sink_sub_bin_webrtc%d", uid);
  bin->sink = gst_element_factory_make("webrtcbin", elem_name);
  if (!bin->sink) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }
  g_object_set(G_OBJECT(bin->sink), "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, NULL);
  if (config->webrtc_stun_server && *config->webrtc_stun_server) {
    g_object_set(G_OBJECT(bin->sink), "stun-server", config->webrtc_stun_server, NULL);
  }
  g_signal_emit_by_name(
      bin->sink, "add-transceiver", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, rtp_caps, &transceiver);
  if (!transceiver) {
    NVGSTDS_ERR_MSG_V("Failed to add WebRTC video transceiver");
    goto done;
  }

  resize_output = config->width && config->height;
  caps_string =
      config->enc_type == NV_DS_ENCODER_TYPE_SW ? "video/x-raw, format=I420" : "video/x-raw(memory:NVMM), format=I420";
  if (resize_output) {
    caps_string += ", width=(int)" + std::to_string(config->width) + ", height=(int)" + std::to_string(config->height);
  }
  raw_caps = gst_caps_from_string(caps_string.c_str());
  g_object_set(G_OBJECT(bin->cap_filter), "caps", raw_caps, NULL);

  if (config->enc_type == NV_DS_ENCODER_TYPE_SW) {
    g_object_set(G_OBJECT(bin->encoder), "bitrate", config->bitrate / 1000, NULL);
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(bin->encoder), "key-int-max")) {
      g_object_set(G_OBJECT(bin->encoder), "key-int-max", iframe_interval, NULL);
    }
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(bin->encoder), "tune")) {
      g_object_set(G_OBJECT(bin->encoder), "tune", 0x00000004, NULL);
    }
  } else {
    g_object_set(G_OBJECT(bin->encoder), "bitrate", config->bitrate, NULL);
    g_object_set(G_OBJECT(bin->encoder), "profile", config->profile, NULL);
    g_object_set(G_OBJECT(bin->encoder), "iframeinterval", iframe_interval, NULL);
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(bin->encoder), "idrinterval")) {
      g_object_set(G_OBJECT(bin->encoder), "idrinterval", iframe_interval, NULL);
    }
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(bin->encoder), "insert-sps-pps")) {
      g_object_set(G_OBJECT(bin->encoder), "insert-sps-pps", 1, NULL);
    }
  }

  cudaGetDeviceProperties(&prop, config->gpu_id);

  if (prop.integrated) {
    if (config->enc_type == NV_DS_ENCODER_TYPE_HW) {
      g_object_set(G_OBJECT(bin->encoder), "preset-level", 1, NULL);
      g_object_set(G_OBJECT(bin->encoder), "insert-sps-pps", 1, NULL);
      g_object_set(G_OBJECT(bin->encoder), "gpu-id", config->gpu_id, NULL);
    }
  } else {
    g_object_set(G_OBJECT(bin->transform), "gpu-id", config->gpu_id, NULL);
  }

  gst_bin_add_many(
      GST_BIN(bin->bin),
      bin->queue,
      bin->transform,
      bin->cap_filter,
      bin->encoder,
      bin->codecparse,
      bin->rtppay_or_flvmux,
      bin->enc_caps_filter,
      bin->sink,
      NULL);

  NVGSTDS_LINK_ELEMENT(bin->queue, bin->transform);
  NVGSTDS_LINK_ELEMENT(bin->transform, bin->cap_filter);
  NVGSTDS_LINK_ELEMENT(bin->cap_filter, bin->encoder);
  NVGSTDS_LINK_ELEMENT(bin->encoder, bin->codecparse);
  NVGSTDS_LINK_ELEMENT(bin->codecparse, bin->rtppay_or_flvmux);
  NVGSTDS_LINK_ELEMENT(bin->rtppay_or_flvmux, bin->enc_caps_filter);

  rtp_src_pad = gst_element_get_static_pad(bin->enc_caps_filter, "src");
  webrtc_sink_pad = find_webrtc_transceiver_sink_pad(bin->sink, transceiver);
  if (!webrtc_sink_pad) {
    webrtc_sink_pad_template = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(bin->sink), "sink_%u");
  }
  if (!webrtc_sink_pad && webrtc_sink_pad_template) {
    webrtc_sink_pad = gst_element_request_pad(bin->sink, webrtc_sink_pad_template, NULL, rtp_caps);
  }
  if (!webrtc_sink_pad) {
    webrtc_sink_pad = gst_element_request_pad_simple(bin->sink, "sink_%u");
  }
  if (rtp_src_pad && webrtc_sink_pad) {
    link_result = gst_pad_link(rtp_src_pad, webrtc_sink_pad);
  }
  if (!rtp_src_pad || !webrtc_sink_pad || link_result != GST_PAD_LINK_OK) {
    gchar* src_caps_string = nullptr;
    gchar* sink_caps_string = nullptr;
    GstCaps* src_caps = rtp_src_pad ? gst_pad_query_caps(rtp_src_pad, NULL) : nullptr;
    GstCaps* sink_caps = webrtc_sink_pad ? gst_pad_query_caps(webrtc_sink_pad, NULL) : nullptr;
    if (src_caps) {
      src_caps_string = gst_caps_to_string(src_caps);
      gst_caps_unref(src_caps);
    }
    if (sink_caps) {
      sink_caps_string = gst_caps_to_string(sink_caps);
      gst_caps_unref(sink_caps);
    }
    g_printerr(
        "Failed to link WebRTC RTP payloader to webrtcbin: src_pad=%p sink_pad=%p result=%s src_caps=%s "
        "sink_caps=%s\n",
        rtp_src_pad,
        webrtc_sink_pad,
        gst_pad_link_get_name(link_result),
        src_caps_string ? src_caps_string : "(none)",
        sink_caps_string ? sink_caps_string : "(none)");
    if (src_caps_string) {
      g_free(src_caps_string);
    }
    if (sink_caps_string) {
      g_free(sink_caps_string);
    }
    goto done;
  }

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->queue, "sink");

  ret = TRUE;
done:
  if (rtp_src_pad) {
    gst_object_unref(rtp_src_pad);
  }
  if (webrtc_sink_pad) {
    gst_object_unref(webrtc_sink_pad);
  }
  if (transceiver) {
    gst_object_unref(transceiver);
  }
  if (raw_caps) {
    gst_caps_unref(raw_caps);
  }
  if (rtp_caps) {
    gst_caps_unref(rtp_caps);
  }
  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }
  return ret;
}

gboolean create_sink_bin(guint num_sub_bins, NvDsSinkSubBinConfig* config_array, NvDsSinkBin* bin, guint index) {
  gboolean ret = FALSE;
  gboolean has_stitched_file = FALSE;
  guint i;
  guint normal_num_bins = 0;
  std::set<guint> rtsp_udp_ports;

  bin->bin = gst_bin_new("sink_bin");
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create element 'sink_bin'");
    goto done;
  }

  bin->queue = gst_element_factory_make(NVDS_ELEM_QUEUE, "sink_bin_queue");
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V("Failed to create element 'sink_bin_queue'");
    goto done;
  }

  gst_bin_add(GST_BIN(bin->bin), bin->queue);

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->queue, "sink");

  bin->tee = gst_element_factory_make(NVDS_ELEM_TEE, "sink_bin_tee");
  if (!bin->tee) {
    NVGSTDS_ERR_MSG_V("Failed to create element 'sink_bin_tee'");
    goto done;
  }

  gst_bin_add(GST_BIN(bin->bin), bin->tee);

  NVGSTDS_LINK_ELEMENT(bin->queue, bin->tee);

  g_object_set(G_OBJECT(bin->tee), "allow-not-linked", TRUE, NULL);

  has_stitched_file =
      std::any_of(config_array, config_array + num_sub_bins, [index](const NvDsSinkSubBinConfig& config) {
        return config.enable && config.source_id == index && !config.link_to_demux &&
            config.type == NV_DS_SINK_ENCODE_STITCHED_FILE;
      });
  if (has_stitched_file) {
    bin->stitched_queue = gst_element_factory_make(NVDS_ELEM_QUEUE, "stitched_sink_bin_queue");
    bin->stitched_tee = gst_element_factory_make(NVDS_ELEM_TEE, "stitched_sink_bin_tee");
    if (!bin->stitched_queue || !bin->stitched_tee) {
      NVGSTDS_ERR_MSG_V("Failed to create stitched archive input route");
      goto done;
    }
    gst_bin_add_many(GST_BIN(bin->bin), bin->stitched_queue, bin->stitched_tee, NULL);
    NVGSTDS_LINK_ELEMENT(bin->stitched_queue, bin->stitched_tee);
    g_object_set(G_OBJECT(bin->stitched_tee), "allow-not-linked", TRUE, NULL);
    NVGSTDS_BIN_ADD_GHOST_PAD_NAMED(bin->bin, bin->stitched_queue, "sink", "stitched_sink");
  }

  for (i = 0; i < num_sub_bins; i++) {
    if (!config_array[i].enable || config_array[i].source_id != index || config_array[i].link_to_demux ||
        config_array[i].type != NV_DS_SINK_UDPSINK) {
      continue;
    }
    if (get_server_sink_type(config_array[i].encoder_config.output_file_path) == SST_RTMP) {
      continue;
    }
    const guint video_port = config_array[i].encoder_config.udp_port;
    if (!rtsp_udp_ports.insert(video_port).second) {
      NVGSTDS_ERR_MSG_V("Duplicate RTSP UDP video port %u", video_port);
      goto done;
    }
    if (rtsp_audio_enabled_for_sink_id(config_array[i].sink_id)) {
      const guint audio_port = video_port + hm::kRtspAudioUdpPortOffset;
      if (!rtsp_udp_ports.insert(audio_port).second) {
        NVGSTDS_ERR_MSG_V("RTSP UDP audio port %u collides with another configured RTSP UDP port", audio_port);
        goto done;
      }
    }
  }

  for (i = 0; i < num_sub_bins; i++) {
    if (!config_array[i].enable) {
      continue;
    }
    if (config_array[i].source_id != index) {
      continue;
    }
    if (config_array[i].link_to_demux) {
      continue;
    }
    switch (config_array[i].type) {
#ifndef IS_TEGRA
      case NV_DS_SINK_RENDER_EGL:
#else
      case NV_DS_SINK_RENDER_3D:
#endif
      case NV_DS_SINK_RENDER_DRM:
      case NV_DS_SINK_FAKE:
        config_array[i].render_config.type = config_array[i].type;
        config_array[i].render_config.sync = config_array[i].sync;
        if (!create_render_bin(&config_array[i].render_config, &bin->sub_bins[i]))
          goto done;
        break;
      case NV_DS_SINK_ENCODE_FILE:
        config_array[i].encoder_config.sync = config_array[i].sync;
        if (!create_encode_file_bin(&config_array[i].encoder_config, &bin->sub_bins[i]))
          goto done;
        break;
      case NV_DS_SINK_ENCODE_STITCHED_FILE:
        config_array[i].encoder_config.sync = config_array[i].sync;
        if (!create_encode_file_bin(
                &config_array[i].encoder_config, &bin->sub_bins[i], TRUE, config_array[i].encoder_config.profile == 1))
          goto done;
        break;
      case NV_DS_SINK_UDPSINK:
        config_array[i].encoder_config.sync = config_array[i].sync;
        if (!create_udpsink_bin(
                &config_array[i].encoder_config,
                &bin->sub_bins[i],
                rtsp_audio_enabled_for_sink_id(config_array[i].sink_id)))
          goto done;
        break;
      case NV_DS_SINK_WEBRTC:
        config_array[i].encoder_config.sync = config_array[i].sync;
        if (!create_webrtc_sink_bin(&config_array[i].encoder_config, &bin->sub_bins[i]))
          goto done;
        break;
      case NV_DS_SINK_MSG_CONV_BROKER:
        config_array[i].msg_conv_broker_config.sync = config_array[i].sync;
        if (!create_msg_conv_broker_bin(&config_array[i].msg_conv_broker_config, &bin->sub_bins[i]))
          goto done;
        break;
      default:
        goto done;
    }

    if (config_array[i].type != NV_DS_SINK_MSG_CONV_BROKER) {
      gst_bin_add(GST_BIN(bin->bin), bin->sub_bins[i].bin);
      GstElement* route_tee = config_array[i].type == NV_DS_SINK_ENCODE_STITCHED_FILE ? bin->stitched_tee : bin->tee;
      if (!route_tee || !link_element_to_tee_src_pad(route_tee, bin->sub_bins[i].bin)) {
        goto done;
      }
    }
    bin->num_bins++;
    if (config_array[i].type != NV_DS_SINK_ENCODE_STITCHED_FILE && config_array[i].type != NV_DS_SINK_MSG_CONV_BROKER) {
      normal_num_bins++;
    }
  }

  if (normal_num_bins == 0) {
    NvDsSinkRenderConfig config;
    memset(&config, 0, sizeof(config));
    config.type = NV_DS_SINK_FAKE;
    guint fallback_index = 0;
    while (fallback_index < MAX_SINK_BINS && bin->sub_bins[fallback_index].bin) {
      ++fallback_index;
    }
    if (fallback_index >= MAX_SINK_BINS || !create_render_bin(&config, &bin->sub_bins[fallback_index]))
      goto done;
    gst_bin_add(GST_BIN(bin->bin), bin->sub_bins[fallback_index].bin);
    if (!link_element_to_tee_src_pad(bin->tee, bin->sub_bins[fallback_index].bin)) {
      goto done;
    }
    bin->num_bins++;
  }

  ret = TRUE;
done:
  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }
  return ret;
}

gboolean create_demux_sink_bin(guint num_sub_bins, NvDsSinkSubBinConfig* config_array, NvDsSinkBin* bin, guint index) {
  gboolean ret = FALSE;
  guint i;

  bin->bin = gst_bin_new("sink_bin");
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create element 'sink_bin'");
    goto done;
  }

  bin->queue = gst_element_factory_make(NVDS_ELEM_QUEUE, "sink_bin_queue");
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V("Failed to create element 'sink_bin_queue'");
    goto done;
  }

  gst_bin_add(GST_BIN(bin->bin), bin->queue);

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->queue, "sink");

  bin->tee = gst_element_factory_make(NVDS_ELEM_TEE, "sink_bin_tee");
  if (!bin->tee) {
    NVGSTDS_ERR_MSG_V("Failed to create element 'sink_bin_tee'");
    goto done;
  }

  gst_bin_add(GST_BIN(bin->bin), bin->tee);

  NVGSTDS_LINK_ELEMENT(bin->queue, bin->tee);

  for (i = 0; i < num_sub_bins; i++) {
    if (!config_array[i].enable) {
      continue;
    }
    if (!config_array[i].link_to_demux) {
      continue;
    }
    switch (config_array[i].type) {
#ifndef IS_TEGRA
      case NV_DS_SINK_RENDER_EGL:
#else
      case NV_DS_SINK_RENDER_3D:
#endif
      case NV_DS_SINK_RENDER_DRM:
      case NV_DS_SINK_FAKE:
        config_array[i].render_config.type = config_array[i].type;
        config_array[i].render_config.sync = config_array[i].sync;
        if (!create_render_bin(&config_array[i].render_config, &bin->sub_bins[i]))
          goto done;
        break;
      case NV_DS_SINK_ENCODE_FILE:
        config_array[i].encoder_config.sync = config_array[i].sync;
        if (!create_encode_file_bin(&config_array[i].encoder_config, &bin->sub_bins[i]))
          goto done;
        break;
      case NV_DS_SINK_UDPSINK:
        if (!create_udpsink_bin(
                &config_array[i].encoder_config,
                &bin->sub_bins[i],
                rtsp_audio_enabled_for_sink_id(config_array[i].sink_id)))
          goto done;
        break;
      case NV_DS_SINK_WEBRTC:
        if (!create_webrtc_sink_bin(&config_array[i].encoder_config, &bin->sub_bins[i]))
          goto done;
        break;
      case NV_DS_SINK_MSG_CONV_BROKER:
        config_array[i].msg_conv_broker_config.sync = config_array[i].sync;
        if (!create_msg_conv_broker_bin(&config_array[i].msg_conv_broker_config, &bin->sub_bins[i]))
          goto done;
        break;
      default:
        goto done;
    }

    if (config_array[i].type != NV_DS_SINK_MSG_CONV_BROKER) {
      gst_bin_add(GST_BIN(bin->bin), bin->sub_bins[i].bin);
      if (!link_element_to_tee_src_pad(bin->tee, bin->sub_bins[i].bin)) {
        goto done;
      }
    }
    bin->num_bins++;
  }

  if (bin->num_bins == 0) {
    NvDsSinkRenderConfig config;
    config.type = NV_DS_SINK_FAKE;
    if (!create_render_bin(&config, &bin->sub_bins[0]))
      goto done;
    gst_bin_add(GST_BIN(bin->bin), bin->sub_bins[0].bin);
    if (!link_element_to_tee_src_pad(bin->tee, bin->sub_bins[0].bin)) {
      goto done;
    }
    bin->num_bins = 1;
  }

  ret = TRUE;
done:
  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }
  return ret;
}

static GstRTSPFilterResult client_filter(GstRTSPServer* server, GstRTSPClient* client, gpointer user_data) {
  return GST_RTSP_FILTER_REMOVE;
}

void destroy_sink_bin() {
  GstRTSPMountPoints* mounts;
  GstRTSPSessionPool* pool;
  guint i = 0;
  destroy_webrtc_signal_servers();
  for (i = 0; i < server_count; i++) {
    mounts = gst_rtsp_server_get_mount_points(server[i]);
    gst_rtsp_mount_points_remove_factory(mounts, "/ds-test");
    g_object_unref(mounts);
    gst_rtsp_server_client_filter(server[i], client_filter, NULL);
    pool = gst_rtsp_server_get_session_pool(server[i]);
    gst_rtsp_session_pool_cleanup(pool);
    g_object_unref(pool);
  }
}
