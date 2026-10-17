#include "WebRtcEndpoint.h"

#include <gst/app/gstappsrc.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {
void expect(bool condition) {
  if (!condition) {
    std::fputs("WebRTC endpoint check failed\n", stderr);
    std::abort();
  }
}
void expect_eos(GstElement* pipeline) {
  GstBus* bus = gst_element_get_bus(pipeline);
  GstMessage* message =
      gst_bus_timed_pop_filtered(bus, 3 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    GError* error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    g_printerr("%s: %s\n", error->message, debug ? debug : "");
    g_clear_error(&error);
    g_free(debug);
  }
  expect(message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS);
  gst_message_unref(message);
  gst_object_unref(bus);
}
void paused_drain_replacement() {
  GstElement* pipeline = gst_pipeline_new(nullptr);
  GstElement* source = gst_element_factory_make("appsrc", nullptr);
  GstElement* endpoint = hstream::webrtc_endpoint_create("paused-endpoint", nullptr);
  GstCaps* caps = gst_caps_from_string("application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000");
  GstPad* input = hstream::webrtc_endpoint_add_track(endpoint, caps);
  expect(input);
  g_object_set(source, "caps", caps, "format", GST_FORMAT_TIME, nullptr);
  gst_caps_unref(caps);
  gst_bin_add_many(GST_BIN(pipeline), source, endpoint, nullptr);
  expect(gst_element_link_pads(source, "src", endpoint, "sink_0"));
  expect(gst_element_set_state(pipeline, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE);
  for (int replacement = 0; replacement < 2; ++replacement) {
    GstPad* valve_input = gst_ghost_pad_get_target(GST_GHOST_PAD(input));
    GstElement* valve = gst_pad_get_parent_element(valve_input);
    GstPad* valve_output = gst_element_get_static_pad(valve, "src");
    GstPad* drain_input = gst_pad_get_peer(valve_output);
    GstElement* drain = gst_pad_get_parent_element(drain_input);
    std::atomic<bool> prerolled{false};
    g_object_set(drain, "signal-handoffs", TRUE, nullptr);
    g_signal_connect(
        drain,
        "preroll-handoff",
        G_CALLBACK(+[](GstElement*, GstBuffer*, GstPad*, gpointer data) {
          static_cast<std::atomic<bool>*>(data)->store(true);
        }),
        &prerolled);
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, 16, nullptr);
    GST_BUFFER_PTS(buffer) = 0;
    expect(gst_app_src_push_buffer(GST_APP_SRC(source), buffer) == GST_FLOW_OK);
    for (int attempt = 0; attempt < 200 && !prerolled.load(); ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    expect(prerolled.load());
    // The streaming thread now holds the valve input lock waiting for PLAYING.
    // Replacing the drain must first unblock preroll, without needing Resume
    // on the same main context that is performing this replacement.
    GstElement* peer = hstream::webrtc_endpoint_reset_peer(endpoint);
    expect(peer);
    hstream::webrtc_endpoint_detach_peer(endpoint);
    gst_object_unref(peer);
    gst_object_unref(drain);
    gst_object_unref(drain_input);
    gst_object_unref(valve_output);
    gst_object_unref(valve);
    gst_object_unref(valve_input);
  }
  expect(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
  gst_app_src_end_of_stream(GST_APP_SRC(source));
  expect_eos(pipeline);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(input);
  gst_object_unref(pipeline);
}

void run(bool replace, bool detach) {
  GstElement* pipeline = gst_pipeline_new(nullptr);
  GstElement* source = gst_element_factory_make("appsrc", nullptr);
  GstElement* audio = gst_element_factory_make("appsrc", nullptr);
  GstElement* endpoint = hstream::webrtc_endpoint_create("endpoint", nullptr);
  GstCaps* caps = gst_caps_from_string("application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000");
  GstPad* ghost = hstream::webrtc_endpoint_add_track(endpoint, caps);
  expect(ghost);
  caps = gst_caps_make_writable(caps);
  gst_caps_set_simple(
      caps,
      "payload",
      G_TYPE_INT,
      112,
      "packetization-mode",
      G_TYPE_STRING,
      "1",
      "profile-level-id",
      G_TYPE_STRING,
      "42c00b",
      "ssrc",
      G_TYPE_UINT,
      12345u,
      nullptr);
  g_object_set(source, "caps", caps, "is-live", TRUE, "format", GST_FORMAT_TIME, nullptr);
  gst_caps_unref(caps);
  GstCaps* audio_caps =
      gst_caps_from_string("application/x-rtp,media=audio,encoding-name=OPUS,payload=97,clock-rate=48000");
  GstPad* audio_ghost = hstream::webrtc_endpoint_add_track(endpoint, audio_caps);
  expect(audio_ghost);
  audio_caps = gst_caps_make_writable(audio_caps);
  gst_caps_set_simple(audio_caps, "payload", G_TYPE_INT, 113, nullptr);
  g_object_set(audio, "caps", audio_caps, "is-live", TRUE, "format", GST_FORMAT_TIME, nullptr);
  gst_caps_unref(audio_caps);
  gst_bin_add_many(GST_BIN(pipeline), source, audio, endpoint, nullptr);
  expect(gst_element_link_pads(audio, "src", endpoint, "sink_1"));
  expect(gst_element_link_pads(source, "src", endpoint, "sink_0"));
  expect(!hstream::webrtc_endpoint_prepare_offer(endpoint));
  expect(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
  auto push = [&] {
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, 16, nullptr);
    GST_BUFFER_PTS(buffer) = 0;
    expect(gst_app_src_push_buffer(GST_APP_SRC(source), buffer) == GST_FLOW_OK);
  };
  push();
  GstBuffer* audio_buffer = gst_buffer_new_allocate(nullptr, 16, nullptr);
  GST_BUFFER_PTS(audio_buffer) = 0;
  expect(gst_app_src_push_buffer(GST_APP_SRC(audio), audio_buffer) == GST_FLOW_OK);
  if (replace) {
    GstElement* previous = nullptr;
    for (int i = 0; i < 5; ++i) {
      GstElement* peer = hstream::webrtc_endpoint_reset_peer(endpoint);
      expect(peer && peer != previous);
      GstWebRTCSessionDescription* description = nullptr;
      g_object_get(peer, "local-description", &description, nullptr);
      expect(!description);
      bool ready = false;
      for (int attempt = 0; attempt < 100 && !ready; ++attempt) {
        ready = hstream::webrtc_endpoint_prepare_offer(endpoint);
        if (!ready)
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      expect(ready);
      GstPromise* promise = gst_promise_new();
      g_signal_emit_by_name(peer, "create-offer", nullptr, promise);
      expect(gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED);
      const GstStructure* reply = gst_promise_get_reply(promise);
      expect(gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &description, nullptr));
      expect(gst_sdp_message_medias_len(description->sdp) == 2);
      expect(std::string(gst_sdp_media_get_format(gst_sdp_message_get_media(description->sdp, 0), 0)) == "112");
      expect(std::string(gst_sdp_media_get_format(gst_sdp_message_get_media(description->sdp, 1), 0)) == "113");
      const char* fmtp = gst_sdp_media_get_attribute_val(gst_sdp_message_get_media(description->sdp, 0), "fmtp");
      expect(fmtp && std::string(fmtp).find("profile-level-id=42c00b") != std::string::npos);
      gst_webrtc_session_description_free(description);
      gst_promise_unref(promise);
      GstPad* current = gst_element_get_static_pad(endpoint, "sink_0");
      expect(current == ghost);
      gst_object_unref(current);
      push();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (previous)
        gst_object_unref(previous);
      previous = peer;
    }
    gst_app_src_end_of_stream(GST_APP_SRC(audio));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    GstObject* parent = gst_object_get_parent(GST_OBJECT(previous));
    expect(parent == GST_OBJECT(endpoint)); // Audio EOF must not end video.
    gst_object_unref(parent);
    if (detach)
      hstream::webrtc_endpoint_detach_peer(endpoint);
    gst_object_unref(previous);
    push();
  }
  gst_app_src_end_of_stream(GST_APP_SRC(source));
  gst_app_src_end_of_stream(GST_APP_SRC(audio));
  expect_eos(pipeline);
  expect(hstream::webrtc_endpoint_reset_peer(endpoint) == nullptr);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(ghost);
  gst_object_unref(audio_ghost);
  gst_object_unref(pipeline);
}
} // namespace
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  paused_drain_replacement();
  run(false, false);
  run(true, true);
  run(true, false);
}
