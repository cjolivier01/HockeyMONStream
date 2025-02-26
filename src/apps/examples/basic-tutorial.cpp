#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h> // For RTSP mode if needed
#include <opencv2/opencv.hpp>
#include <cstring> // for strcmp
#include <iostream>
#include <string>

// Structure used for linking dynamic demux pads.
struct DemuxData {
  GstElement* videoQueue;
  GstElement* audioQueue;
};

// Structure for counting frames.
struct FrameCounterData {
  gint count;
  gint maxFrames; // 0 means no limit.
  GstElement* pipeline;
};

static GstPadProbeReturn frame_count_probe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
  FrameCounterData* counterData = static_cast<FrameCounterData*>(user_data);
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
    counterData->count++;
    g_print("Frame %d processed.\n", counterData->count);
    if (counterData->maxFrames > 0 && counterData->count >= counterData->maxFrames) {
      g_print("Maximum frame count (%d) reached. Sending EOS.\n", counterData->maxFrames);
      gst_element_send_event(counterData->pipeline, gst_event_new_eos());
      return GST_PAD_PROBE_REMOVE;
    }
  }
  return GST_PAD_PROBE_OK;
}

static void on_pad_added(GstElement* src, GstPad* new_pad, gpointer user_data) {
  DemuxData* data = static_cast<DemuxData*>(user_data);
  GstCaps* caps = gst_pad_get_current_caps(new_pad);
  if (!caps)
    caps = gst_pad_query_caps(new_pad, NULL);
  const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
  if (g_str_has_prefix(name, "video/")) {
    GstPad* sink_pad = gst_element_get_static_pad(data->videoQueue, "sink");
    if (gst_pad_is_linked(sink_pad)) {
      g_print("Video pad already linked; ignoring.\n");
      gst_object_unref(sink_pad);
      gst_caps_unref(caps);
      return;
    }
    if (gst_pad_link(new_pad, sink_pad) != GST_PAD_LINK_OK)
      g_printerr("Failed to link video pad.\n");
    else
      g_print("Video pad linked.\n");
    gst_object_unref(sink_pad);
  } else if (g_str_has_prefix(name, "audio/")) {
    GstPad* sink_pad = gst_element_get_static_pad(data->audioQueue, "sink");
    if (gst_pad_is_linked(sink_pad)) {
      g_print("Audio pad already linked; ignoring.\n");
      gst_object_unref(sink_pad);
      gst_caps_unref(caps);
      return;
    }
    if (gst_pad_link(new_pad, sink_pad) != GST_PAD_LINK_OK)
      g_printerr("Failed to link audio pad.\n");
    else
      g_print("Audio pad linked.\n");
    gst_object_unref(sink_pad);
  }
  gst_caps_unref(caps);
}

static gboolean bus_call(GstBus* bus, GstMessage* msg, gpointer data) {
  GMainLoop* loop = static_cast<GMainLoop*>(data);
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
      g_print("End-of-stream\n");
      g_main_loop_quit(loop);
      break;
    case GST_MESSAGE_ERROR: {
      GError* err = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(msg, &err, &debug);
      g_printerr("Error: %s\n", err->message);
      g_error_free(err);
      g_free(debug);
      g_main_loop_quit(loop);
      break;
    }
    case GST_MESSAGE_STATE_CHANGED: {
      GstElement* elem = GST_ELEMENT(msg->src);
      GstState old_state, new_state, pending_state;
      gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
      g_print(
          "Element %s changed state from %s to %s.\n",
          GST_ELEMENT_NAME(elem),
          gst_element_state_get_name(old_state),
          gst_element_state_get_name(new_state));
      break;
    }
    default:
      break;
  }
  return TRUE;
}

int main(int argc, char* argv[]) {
  gst_init(&argc, &argv);

  // Command-line usage:
  //   deepstream_app <input file> <output file> [--rtsp] [--maxframes=<number>] [--bitrate=<value>] [--lossless]
  if (argc < 3) {
    g_printerr(
        "Usage: %s <input file> <output file> [--rtsp] [--maxframes=<number>] [--bitrate=<value>] [--lossless]\n",
        argv[0]);
    return -1;
  }
  std::string inputFilename = argv[1];
  std::string outputFilename = argv[2];
  bool rtspMode = false;
  gint maxFrames = 0; // 0 means process all frames.
  gint bitrate = 0; // 0 means not set.
  bool lossless = false;
  // Parse additional arguments.
  for (int i = 3; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--rtsp")
      rtspMode = true;
    else if (arg.find("--maxframes=") == 0) {
      std::string numStr = arg.substr(strlen("--maxframes="));
      maxFrames = std::stoi(numStr);
    } else if (arg.find("--bitrate=") == 0) {
      std::string numStr = arg.substr(strlen("--bitrate="));
      bitrate = std::stoi(numStr);
    } else if (arg == "--lossless") {
      lossless = true;
    }
  }

  // Query video dimensions using OpenCV.
  cv::VideoCapture cap(inputFilename);
  if (!cap.isOpened()) {
    std::cerr << "Error opening video file " << inputFilename << std::endl;
    return -1;
  }
  int origWidth = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
  int origHeight = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
  int newWidth = origWidth / 2;
  int newHeight = origHeight / 2;
  std::cout << "Original size: " << origWidth << "x" << origHeight << ", New size: " << newWidth << "x" << newHeight
            << std::endl;
  cap.release();

  if (rtspMode) {
    g_print("RTSP mode not implemented in this example.\n");
    return 0;
  }

  GMainLoop* loop = g_main_loop_new(NULL, FALSE);
  GstElement* pipeline = gst_pipeline_new("deepstream-pipeline");

  // Create source and demuxer.
  GstElement* source = gst_element_factory_make("filesrc", "source");
  GstElement* demux = gst_element_factory_make("qtdemux", "demux");

  // Video branch elements.
  GstElement* videoQueue = gst_element_factory_make("queue", "videoQueue");
  GstElement* h265parse = gst_element_factory_make("h265parse", "h265parse");
  GstElement* decoder = gst_element_factory_make("avdec_h265", "decoder");
  GstElement* vidConvert = gst_element_factory_make("nvvideoconvert", "vidConvert");
  GstElement* capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
  GstElement* encoder = gst_element_factory_make("nvv4l2h265enc", "encoder");

  // Insert a second parser after the encoder.
  GstElement* postParse = gst_element_factory_make("h265parse", "postParse");

  // Audio branch elements.
  GstElement* audioQueue = gst_element_factory_make("queue", "audioQueue");
  GstElement* aacparse = gst_element_factory_make("aacparse", "aacparse");

  // Muxer and sink.
  GstElement* muxer = gst_element_factory_make("mp4mux", "muxer");
  GstElement* sink = gst_element_factory_make("filesink", "sink");

  if (!pipeline || !source || !demux || !videoQueue || !h265parse || !decoder || !vidConvert || !capsfilter ||
      !encoder || !postParse || !audioQueue || !aacparse || !muxer || !sink) {
    g_printerr("Not all elements could be created.\n");
    return -1;
  }

  // Set basic properties.
  g_object_set(G_OBJECT(source), "location", inputFilename.c_str(), NULL);
  g_object_set(G_OBJECT(sink), "location", outputFilename.c_str(), NULL);

  // Create caps for the video branch and set NVMM.
  GstCaps* caps = gst_caps_new_simple(
      "video/x-raw",
      "format",
      G_TYPE_STRING,
      "NV12",
      "width",
      G_TYPE_INT,
      newWidth,
      "height",
      G_TYPE_INT,
      newHeight,
      NULL);
  gst_caps_set_features(caps, 0, gst_caps_features_from_string("memory:NVMM"));
  g_object_set(G_OBJECT(capsfilter), "caps", caps, NULL);
  gst_caps_unref(caps);

  // Configure the encoder based on bitrate or lossless option.
  if (lossless) {
    g_print("Configuring encoder for lossless encoding.\n");
    // For lossless, assume the encoder supports a "rc-mode" property.
    // (The enum value used here is hypothetical; adjust according to your plugin.)
    g_object_set(G_OBJECT(encoder), "rc-mode", 2, NULL); // Assume 2 => lossless mode.
    // Optionally, you might also set the quantization parameter to 0.
    g_object_set(G_OBJECT(encoder), "qp", 0, NULL);
  } else if (bitrate > 0) {
    g_print("Setting encoder bitrate to %d kbit/s.\n", bitrate);
    g_object_set(G_OBJECT(encoder), "bitrate", bitrate, NULL);
    // Optionally, set a rate-control mode (e.g. CBR).
    g_object_set(G_OBJECT(encoder), "rc-mode", 1, NULL); // Assume 1 => CBR.
  }

  // Add all elements to the pipeline.
  gst_bin_add_many(
      GST_BIN(pipeline),
      source,
      demux,
      videoQueue,
      h265parse,
      decoder,
      vidConvert,
      capsfilter,
      encoder,
      postParse,
      audioQueue,
      aacparse,
      muxer,
      sink,
      NULL);

  // Link static elements.
  if (!gst_element_link(source, demux)) {
    g_printerr("Failed to link source to demux.\n");
    return -1;
  }
  if (!gst_element_link_many(videoQueue, h265parse, decoder, vidConvert, capsfilter, encoder, postParse, NULL)) {
    g_printerr("Failed to link video branch elements.\n");
    return -1;
  }
  if (!gst_element_link_many(audioQueue, aacparse, NULL)) {
    g_printerr("Failed to link audio branch elements.\n");
    return -1;
  }
  if (!gst_element_link(muxer, sink)) {
    g_printerr("Failed to link muxer to sink.\n");
    return -1;
  }

  // Connect demuxer's pad-added signal.
  DemuxData demuxData = {videoQueue, audioQueue};
  g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), &demuxData);

  // Link the video branch to the muxer.
  GstPad* postParse_src = gst_element_get_static_pad(postParse, "src");
  if (!postParse_src) {
    g_printerr("Could not get postParse src pad.\n");
    return -1;
  }
  GstPad* muxer_video_pad = gst_element_request_pad_simple(muxer, "video_%u");
  if (!muxer_video_pad) {
    g_printerr("Could not get request pad from muxer for video.\n");
    return -1;
  }
  if (gst_pad_link(postParse_src, muxer_video_pad) != GST_PAD_LINK_OK) {
    g_printerr("Failed to link postParse to muxer (video branch).\n");
    return -1;
  }
  gst_object_unref(postParse_src);
  gst_object_unref(muxer_video_pad);

  // Link the audio branch to the muxer.
  GstPad* aac_src = gst_element_get_static_pad(aacparse, "src");
  if (!aac_src) {
    g_printerr("Could not get aacparse src pad.\n");
    return -1;
  }
  GstPad* muxer_audio_pad = gst_element_request_pad_simple(muxer, "audio_%u");
  if (!muxer_audio_pad) {
    g_printerr("Could not get request pad from muxer for audio.\n");
    return -1;
  }
  if (gst_pad_link(aac_src, muxer_audio_pad) != GST_PAD_LINK_OK) {
    g_printerr("Failed to link audio branch to muxer.\n");
    return -1;
  }
  gst_object_unref(aac_src);
  gst_object_unref(muxer_audio_pad);

  // If maxFrames is set, add a probe to the postParse's src pad to count frames.
  FrameCounterData frameData = {0, maxFrames, pipeline};
  if (maxFrames > 0) {
    GstPad* probe_pad = gst_element_get_static_pad(postParse, "src");
    if (!probe_pad) {
      g_printerr("Could not get postParse src pad for probe.\n");
      return -1;
    }
    gst_pad_add_probe(probe_pad, GST_PAD_PROBE_TYPE_BUFFER, frame_count_probe, &frameData, NULL);
    gst_object_unref(probe_pad);
  }

  // Add bus watch.
  GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
  gst_bus_add_watch(bus, bus_call, loop);
  gst_object_unref(bus);

  // Set pipeline state.
  gst_element_set_state(pipeline, GST_STATE_PAUSED);
  gst_element_set_state(pipeline, GST_STATE_PLAYING);

  g_print("Running pipeline...\n");
  g_main_loop_run(loop);

  // Cleanup.
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  g_main_loop_unref(loop);

  return 0;
}
