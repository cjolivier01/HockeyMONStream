#include <gst/gst.h>

// Callback to count video frames and print every frame.
static GstPadProbeReturn video_frame_probe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
  static guint frame_count = 0;
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
    frame_count++;
    // For testing, print every frame (you can change the modulus as needed).
    g_print("Processed %u video frames.\n", frame_count);
  }
  return GST_PAD_PROBE_OK;
}

// Callback: When the demuxer adds a new pad, link it to the proper branch.
static void on_pad_added(GstElement* src, GstPad* new_pad, gpointer data) {
  GstCaps* caps = gst_pad_get_current_caps(new_pad);
  GstStructure* str = gst_caps_get_structure(caps, 0);
  const gchar* new_pad_type = gst_structure_get_name(str);
  GstElement* pipeline = (GstElement*)data;
  GstPad* sink_pad = NULL;

  if (g_str_has_prefix(new_pad_type, "video/")) {
    // Link to video queue.
    GstElement* videoQueue = gst_bin_get_by_name(GST_BIN(pipeline), "video-queue");
    sink_pad = gst_element_get_static_pad(videoQueue, "sink");
    if (gst_pad_is_linked(sink_pad)) {
      g_object_unref(sink_pad);
      gst_object_unref(videoQueue);
      gst_caps_unref(caps);
      return;
    }
    if (gst_pad_link(new_pad, sink_pad) != GST_PAD_LINK_OK)
      g_printerr("Failed to link video pad.\n");
    else
      g_print("Video pad linked successfully.\n");
    g_object_unref(sink_pad);
    gst_object_unref(videoQueue);
  } else if (g_str_has_prefix(new_pad_type, "audio/")) {
    // Link to audio queue.
    GstElement* audioQueue = gst_bin_get_by_name(GST_BIN(pipeline), "audio-queue");
    sink_pad = gst_element_get_static_pad(audioQueue, "sink");
    if (gst_pad_is_linked(sink_pad)) {
      g_object_unref(sink_pad);
      gst_object_unref(audioQueue);
      gst_caps_unref(caps);
      return;
    }
    if (gst_pad_link(new_pad, sink_pad) != GST_PAD_LINK_OK)
      g_printerr("Failed to link audio pad.\n");
    else
      g_print("Audio pad linked successfully.\n");
    g_object_unref(sink_pad);
    gst_object_unref(audioQueue);
  }
  gst_caps_unref(caps);
}

int main(int argc, char* argv[]) {
  gst_init(&argc, &argv);

  if (argc != 3) {
    g_printerr("Usage: %s <input file> <rtmp url>\n", argv[0]);
    return -1;
  }
  const gchar* input_file = argv[1];
  const gchar* rtmp_url = argv[2];

  // Create the empty pipeline.
  GstElement* pipeline = gst_pipeline_new("rtmp-stream-pipeline");

  // Create elements for reading and demuxing.
  GstElement* source = gst_element_factory_make("filesrc", "file-source");
  GstElement* demux = gst_element_factory_make("qtdemux", "demuxer");

  // --- Video Branch ---
  // (Assuming the video is H.264 encoded in the file.)
  GstElement* videoQueue = gst_element_factory_make("queue", "video-queue");
  GstElement* h264parse = gst_element_factory_make("h264parse", "video-h264parse");
  GstElement* videoDecoder = gst_element_factory_make("avdec_h264", "video-decoder");
  GstElement* videoScale = gst_element_factory_make("videoscale", "video-scale");
  GstElement* videoConvert = gst_element_factory_make("videoconvert", "video-convert");
  // Caps filter to downscale video (assumes 1920x1080 input; 25% becomes 480x270).
  GstElement* videoCapsFilter = gst_element_factory_make("capsfilter", "video-caps");
  GstElement* videoEncoder = gst_element_factory_make("x264enc", "video-encoder");
  // Parse the re-encoded video stream.
  GstElement* videoParse = gst_element_factory_make("h264parse", "video-parser");

  // --- Audio Branch ---
  GstElement* audioQueue = gst_element_factory_make("queue", "audio-queue");
  // (Assuming the audio is AAC encoded.)
  GstElement* audioDecoder = gst_element_factory_make("faad", "audio-decoder");
  GstElement* audioConvert = gst_element_factory_make("audioconvert", "audio-convert");
  GstElement* audioResample = gst_element_factory_make("audioresample", "audio-resample");
  GstElement* audioEncoder = gst_element_factory_make("voaacenc", "audio-encoder");
  GstElement* audioParse = gst_element_factory_make("aacparse", "audio-parser");

  // --- Mux and Sink ---
  // Mux audio and video streams into FLV.
  GstElement* muxer = gst_element_factory_make("flvmux", "muxer");
  // Send the FLV stream to the RTMP server.
  GstElement* rtmpSink = gst_element_factory_make("rtmpsink", "rtmp-sink");

  // Check that all elements were created.
  if (!pipeline || !source || !demux || !videoQueue || !h264parse || !videoDecoder || !videoScale || !videoConvert ||
      !videoCapsFilter || !videoEncoder || !videoParse || !audioQueue || !audioDecoder || !audioConvert ||
      !audioResample || !audioEncoder || !audioParse || !muxer || !rtmpSink) {
    g_printerr("One or more elements could not be created. Exiting.\n");
    return -1;
  }

  // Set element properties.
  g_object_set(G_OBJECT(source), "location", input_file, NULL);
  g_object_set(G_OBJECT(rtmpSink), "location", rtmp_url, NULL);

  // Set the video caps filter (downscaling).
  GstCaps* caps = gst_caps_from_string("video/x-raw, width=480, height=270");
  g_object_set(G_OBJECT(videoCapsFilter), "caps", caps, NULL);
  gst_caps_unref(caps);

  // Add all elements into the pipeline.
  // Note: We do NOT include the muxer in the gst_element_link_many() calls below.
  gst_bin_add_many(
      GST_BIN(pipeline),
      source,
      demux,
      videoQueue,
      h264parse,
      videoDecoder,
      videoScale,
      videoConvert,
      videoCapsFilter,
      videoEncoder,
      videoParse,
      audioQueue,
      audioDecoder,
      audioConvert,
      audioResample,
      audioEncoder,
      audioParse,
      muxer,
      rtmpSink,
      NULL);

  // Link the file source to the demuxer.
  if (!gst_element_link(source, demux)) {
    g_printerr("Failed to link source to demuxer.\n");
    gst_object_unref(pipeline);
    return -1;
  }

  // Link the video branch up to the parser.
  if (!gst_element_link_many(
          videoQueue,
          h264parse,
          videoDecoder,
          videoScale,
          videoConvert,
          videoCapsFilter,
          videoEncoder,
          videoParse,
          NULL)) {
    g_printerr("Failed to link video branch elements.\n");
    gst_object_unref(pipeline);
    return -1;
  }

  // Link the audio branch up to the parser.
  if (!gst_element_link_many(audioQueue, audioDecoder, audioConvert, audioResample, audioEncoder, audioParse, NULL)) {
    g_printerr("Failed to link audio branch elements.\n");
    gst_object_unref(pipeline);
    return -1;
  }

  // Link the muxer to the RTMP sink.
  if (!gst_element_link(muxer, rtmpSink)) {
    g_printerr("Failed to link muxer to RTMP sink.\n");
    gst_object_unref(pipeline);
    return -1;
  }

  // Connect the "pad-added" signal of the demuxer to our callback.
  g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), pipeline);

  // Add a pad probe on the src pad of the videoCapsFilter to count frames.
  GstPad* probe_pad = gst_element_get_static_pad(videoCapsFilter, "src");
  gst_pad_add_probe(probe_pad, GST_PAD_PROBE_TYPE_BUFFER, video_frame_probe, NULL, NULL);
  gst_object_unref(probe_pad);

  // IMPORTANT: Manually link the output of the video and audio branches to the muxer.
  // For flvmux, request the "video" pad and link from videoParse.
  GstPad* mux_video_pad = gst_element_request_pad_simple(muxer, "video");
  GstPad* video_src_pad = gst_element_get_static_pad(videoParse, "src");
  if (gst_pad_link(video_src_pad, mux_video_pad) != GST_PAD_LINK_OK) {
    g_printerr("Failed to link video parse to muxer.\n");
  } else {
    g_print("Video branch linked to muxer successfully.\n");
  }
  gst_object_unref(video_src_pad);
  gst_object_unref(mux_video_pad);

  // Request and link the "audio" pad for the audio branch.
  // GstPad* mux_audio_pad = gst_element_request_pad_simple(muxer, "audio");
  // GstPad* audio_src_pad = gst_element_get_static_pad(audioParse, "src");
  // if (gst_pad_link(audio_src_pad, mux_audio_pad) != GST_PAD_LINK_OK) {
  //   g_printerr("Failed to link audio parse to muxer.\n");
  // } else {
  //   g_print("Audio branch linked to muxer successfully.\n");
  // }
  // gst_object_unref(audio_src_pad);
  // gst_object_unref(mux_audio_pad);

  // Start playing the pipeline.
  GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Unable to set the pipeline to the playing state.\n");
    gst_object_unref(pipeline);
    return -1;
  }

  // Wait until error or EOS.
  GstBus* bus = gst_element_get_bus(pipeline);
  GstMessage* msg =
      gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

  // Parse message.
  if (msg != NULL) {
    GError* err;
    gchar* debug_info;
    switch (GST_MESSAGE_TYPE(msg)) {
      case GST_MESSAGE_ERROR:
        gst_message_parse_error(msg, &err, &debug_info);
        g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
        g_printerr("Debug info: %s\n", debug_info ? debug_info : "none");
        g_clear_error(&err);
        g_free(debug_info);
        break;
      case GST_MESSAGE_EOS:
        g_print("End-Of-Stream reached.\n");
        break;
      default:
        g_printerr("Unexpected message received.\n");
        break;
    }
    gst_message_unref(msg);
  }

  // Free resources.
  gst_object_unref(bus);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);

  return 0;
}
