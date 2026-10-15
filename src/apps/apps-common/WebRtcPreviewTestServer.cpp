#include <gst/gst.h>
#include <cstdlib>
#include <string>
#include "WebRtcEndpoint.h"
#include "deepstream_sinks.h"

// CPU-only signaling fixture for browser decode/reconnect acceptance.
GST_DEBUG_CATEGORY(NVDS_APP);
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GError* error = nullptr;
  GstElement* pipeline = gst_parse_launch(
      "videotestsrc is-live=true ! video/x-raw,width=320,height=180,framerate=15/1 ! "
      "videoconvert ! openh264enc complexity=low gop-size=15 ! h264parse ! rtph264pay config-interval=-1 pt=96 ! "
      "application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000,packetization-mode=(string)1 ! identity name=video "
      "audiotestsrc is-live=true ! audioconvert ! opusenc ! rtpopuspay pt=97 ! identity name=audio",
      &error);
  if (!pipeline || error) {
    g_printerr("%s\n", error ? error->message : "pipeline unavailable");
    return 1;
  }
  GstElement* endpoint = hstream::webrtc_endpoint_create("preview", nullptr);
  gst_bin_add(GST_BIN(pipeline), endpoint);
  for (const char* name : {"video", "audio"}) {
    GstElement* source = gst_bin_get_by_name(GST_BIN(pipeline), name);
    GstPad* src = gst_element_get_static_pad(source, "src");
    GstCaps* caps = gst_caps_from_string(
        std::string(name) == "video"
            ? "application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000,packetization-mode=(string)1,profile-level-id=(string)42e01f"
            : "application/x-rtp,media=audio,encoding-name=OPUS,payload=97,clock-rate=48000,encoding-params=(string)2");
    GstPad* sink = hstream::webrtc_endpoint_add_track(endpoint, caps);
    if (!sink || gst_pad_link(src, sink) != GST_PAD_LINK_OK)
      return 2;
    gst_object_unref(sink);
    gst_caps_unref(caps);
    gst_object_unref(src);
    gst_object_unref(source);
  }
  NvDsSinkEncoderConfig config{};
  config.webrtc_port = argc > 1 ? std::atoi(argv[1]) : 18080;
  if (!start_webrtc_signaling_for_sink(endpoint, &config))
    return 3;
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    return 4;
  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  GstBus* bus = gst_element_get_bus(pipeline);
  gst_bus_add_watch(
      bus,
      [](GstBus*, GstMessage* message, gpointer) -> gboolean {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR || GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
          GError* error = nullptr;
          gchar* debug = nullptr;
          if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
            gst_message_parse_error(message, &error, &debug);
          else
            gst_message_parse_warning(message, &error, &debug);
          g_printerr(
              "%s %s: %s (%s)\n",
              GST_MESSAGE_TYPE_NAME(message),
              GST_OBJECT_NAME(message->src),
              error ? error->message : "",
              debug ? debug : "");
          g_clear_error(&error);
          g_free(debug);
        }
        return G_SOURCE_CONTINUE;
      },
      nullptr);
  gst_object_unref(bus);
  GIOChannel* input = g_io_channel_unix_new(0);
  g_io_add_watch(
      input,
      static_cast<GIOCondition>(G_IO_IN | G_IO_HUP),
      [](GIOChannel*, GIOCondition, gpointer data) -> gboolean {
        g_main_loop_quit(static_cast<GMainLoop*>(data));
        return G_SOURCE_REMOVE;
      },
      loop);
  g_print("WEBRTC_TEST_READY port=%u\n", config.webrtc_port);
  g_main_loop_run(loop);
  prepare_webrtc_sinks_for_stop(pipeline);
  gst_element_send_event(pipeline, gst_event_new_eos());
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  g_main_loop_unref(loop);
  g_io_channel_unref(input);
  return 0;
}
