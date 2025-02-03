#include <glib.h>
#include <gst/gst.h>
#include <iostream>

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <mp4-file-path>\n";
    return 1;
  }

  gst_init(&argc, &argv);

  GstElement* pipeline = gst_pipeline_new("mp4-av-pipeline");
  GstElement* source = gst_element_factory_make("filesrc", "file-source");
  GstElement* demuxer = gst_element_factory_make("qtdemux", "demuxer");

  // Video elements
  GstElement* videoQueue = gst_element_factory_make("queue", "video-queue");
  GstElement* videoDecoder = gst_element_factory_make("nvv4l2decoder", "video-decoder");
  GstElement* streammux = gst_element_factory_make("nvstreammux", "stream-muxer");
  GstElement* videoConvert = gst_element_factory_make("nvvideoconvert", "converter");
  GstElement* videoSink = gst_element_factory_make("nveglglessink", "video-renderer");

  // Audio elements
  GstElement* audioQueue = gst_element_factory_make("queue", "audio-queue");
  GstElement* audioDecoder = gst_element_factory_make("avdec_aac", "audio-decoder");
  GstElement* audioConvert = gst_element_factory_make("audioconvert", "aconverter");
  GstElement* audioResample = gst_element_factory_make("audioresample", "resampler");
  GstElement* audioSink = gst_element_factory_make("alsasink", "audio-output");

  if (!pipeline || !source || !demuxer || !videoQueue || !videoDecoder || !streammux || !videoConvert || !videoSink ||
      !audioQueue || !audioDecoder || !audioConvert || !audioResample || !audioSink) {
    std::cerr << "Element creation failed\n";
    return 1;
  }

  // Configure streammux
  g_object_set(
      G_OBJECT(streammux),
      "width",
      1920,
      "height",
      1080,
      "batch-size",
      1,
      "batched-push-timeout",
      40000,
      "live-source",
      0,
      NULL);

  // Configure sync properties
  g_object_set(G_OBJECT(videoSink), "sync", TRUE, NULL);
  g_object_set(G_OBJECT(audioSink), "sync", TRUE, NULL);

  // Configure queues for sync
  g_object_set(G_OBJECT(videoQueue), "max-size-buffers", 4, "max-size-time", 0, "max-size-bytes", 0, NULL);
  g_object_set(G_OBJECT(audioQueue), "max-size-buffers", 4, "max-size-time", 0, "max-size-bytes", 0, NULL);

  g_object_set(G_OBJECT(source), "location", argv[1], NULL);

  gst_bin_add_many(
      GST_BIN(pipeline),
      source,
      demuxer,
      videoQueue,
      videoDecoder,
      streammux,
      videoConvert,
      videoSink,
      audioQueue,
      audioDecoder,
      audioConvert,
      audioResample,
      audioSink,
      NULL);

  // Link pre-demuxer elements
  gst_element_link(source, demuxer);

  // Link video elements after queue
  gst_element_link_many(videoQueue, videoDecoder, NULL);
  gst_element_link_many(streammux, videoConvert, videoSink, NULL);

  // Link audio elements after queue
  gst_element_link_many(audioQueue, audioDecoder, audioConvert, audioResample, audioSink, NULL);

  // Request sink pad from streammux
  GstPad* sinkpad = gst_element_get_request_pad(streammux, "sink_0");
  if (!sinkpad) {
    std::cerr << "Failed to get sink pad from streammux\n";
    return 1;
  }
  gst_object_unref(sinkpad);

  // Connect demuxer pad-added signal
  g_signal_connect(demuxer, "pad-added", G_CALLBACK(on_pad_added), NULL);

  // Set pipeline to PLAYING
  GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to start pipeline\n";
    return 1;
  }

  // Main loop
  GMainLoop* loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(loop);

  // Cleanup
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(GST_OBJECT(pipeline));
  g_main_loop_unref(loop);

  return 0;
}

static void on_pad_added(GstElement* element, GstPad* pad, gpointer data) {
  GstCaps* caps = gst_pad_get_current_caps(pad);
  GstStructure* str = gst_caps_get_structure(caps, 0);
  const gchar* name = gst_structure_get_name(str);

  GstElement* queue = NULL;
  GstElement* streammux = NULL;

  if (g_str_has_prefix(name, "video/")) {
    queue = gst_bin_get_by_name(GST_BIN(gst_element_get_parent(element)), "video-queue");
    streammux = gst_bin_get_by_name(GST_BIN(gst_element_get_parent(element)), "stream-muxer");

    // Get decoder source pad and link to streammux
    GstElement* decoder = gst_bin_get_by_name(GST_BIN(gst_element_get_parent(element)), "video-decoder");
    GstPad* srcpad = gst_element_get_static_pad(decoder, "src");
    GstPad* muxsinkpad = gst_element_get_static_pad(streammux, "sink_0");
    gst_pad_link(srcpad, muxsinkpad);
    gst_object_unref(srcpad);
    gst_object_unref(muxsinkpad);
    gst_object_unref(decoder);

  } else if (g_str_has_prefix(name, "audio/")) {
    queue = gst_bin_get_by_name(GST_BIN(gst_element_get_parent(element)), "audio-queue");
  }

  if (queue) {
    GstPad* sinkpad = gst_element_get_static_pad(queue, "sink");
    gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
    gst_object_unref(queue);
  }

  if (streammux) {
    gst_object_unref(streammux);
  }

  gst_caps_unref(caps);
}
