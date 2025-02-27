#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <opencv2/opencv.hpp>
#include <cstring>
#include <iostream>
#include <string>

// -----------------------------------------------------------------
// Common structures and functions
// -----------------------------------------------------------------

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

// Structure for app-wide data passed to the bus callback.
struct AppData {
  GMainLoop* loop;
  GstElement* pipeline;
};

// Parameters for the RTSP pipeline.
struct RTSPParams {
  char inputFilename[1024];
  int newWidth;
  int newHeight;
  int bitrate; // in kbps; 0 means not set.
  bool lossless;
};

// Probe function to count frames (prints every 250 frames).
static GstPadProbeReturn frame_count_probe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
  FrameCounterData* counterData = static_cast<FrameCounterData*>(user_data);
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
    counterData->count++;
    if (counterData->count % 250 == 0) {
      g_print("Frame %d processed.\n", counterData->count);
    }
    if (counterData->maxFrames > 0 && counterData->count >= counterData->maxFrames) {
      g_print("Maximum frame count (%d) reached. Sending EOS.\n", counterData->maxFrames);
      gst_element_send_event(counterData->pipeline, gst_event_new_eos());
      return GST_PAD_PROBE_REMOVE;
    }
  }
  return GST_PAD_PROBE_OK;
}

// Demuxer pad-added callback.
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

// Bus callback. When the pipeline reaches PAUSED, dump the pipeline graph.
static gboolean bus_call(GstBus* bus, GstMessage* msg, gpointer data) {
  AppData* appData = static_cast<AppData*>(data);
  GMainLoop* loop = appData->loop;
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
      if (GST_MESSAGE_SRC(msg) == GST_OBJECT(appData->pipeline)) {
        GstState old_state, new_state, pending_state;
        gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
        g_print(
            "Pipeline state changed from %s to %s.\n",
            gst_element_state_get_name(old_state),
            gst_element_state_get_name(new_state));
        if (new_state == GST_STATE_PAUSED) {
          gchar* dot_file = g_strdup_printf("pipeline_paused.dot");
          GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(appData->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, dot_file);
          g_print("Pipeline graph dumped to %s\n", dot_file);
          g_free(dot_file);
        }
      }
      break;
    }
    default:
      break;
  }
  return TRUE;
}

// -----------------------------------------------------------------
// File-output mode: Build the pipeline element-by-element (unchanged)
// -----------------------------------------------------------------
// (For brevity, the file-output branch code is similar to previous examples.)

// -----------------------------------------------------------------
// RTSP Mode: Build the RTSP pipeline in C++ (element-by-element)
// -----------------------------------------------------------------

static GstElement* create_rtsp_pipeline(GstRTSPMediaFactory* factory, gpointer user_data) {
  RTSPParams* params = static_cast<RTSPParams*>(user_data);
  // Create a bin to hold the RTSP pipeline.
  GstElement* bin = gst_bin_new("rtsp_bin");
  if (!bin) {
    g_printerr("Failed to create RTSP bin.\n");
    return NULL;
  }

  // Create elements.
  GstElement* source = gst_element_factory_make("filesrc", "source");
  g_object_set(G_OBJECT(source), "location", params->inputFilename.c_str(), NULL);
  GstElement* demux = gst_element_factory_make("qtdemux", "demux");

  // Video branch.
  GstElement* videoQueue = gst_element_factory_make("queue", "videoQueue");
  GstElement* vparse = gst_element_factory_make("h265parse", "vparse");
  GstElement* decoder = gst_element_factory_make("avdec_h265", "decoder");
  GstElement* vidConvert = gst_element_factory_make("nvvideoconvert", "vidConvert");
  GstElement* capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
  GstElement* encoder = gst_element_factory_make("nvv4l2h265enc", "encoder");
  GstElement* postParse = gst_element_factory_make("h265parse", "postParse");
  // RTP payloader for video (will be ghosted as "pay0").
  GstElement* rtpPayVideo = gst_element_factory_make("rtph265pay", "pay0");
  g_object_set(G_OBJECT(rtpPayVideo), "pt", 96, NULL);

  // Audio branch.
  GstElement* audioQueue = gst_element_factory_make("queue", "audioQueue");
  GstElement* aacparse = gst_element_factory_make("aacparse", "aacparse");
  // RTP payloader for audio (will be ghosted as "pay1").
  GstElement* rtpPayAudio = gst_element_factory_make("rtpmp4gpay", "pay1");
  g_object_set(G_OBJECT(rtpPayAudio), "pt", 97, NULL);

  if (!source || !demux || !videoQueue || !vparse || !decoder || !vidConvert || !capsfilter || !encoder || !postParse ||
      !rtpPayVideo || !audioQueue || !aacparse || !rtpPayAudio) {
    g_printerr("RTSP: Failed to create one or more elements.\n");
    return NULL;
  }

  // Configure caps for video branch.
  GstCaps* caps = gst_caps_new_simple(
      "video/x-raw",
      "format",
      G_TYPE_STRING,
      "NV12",
      "width",
      G_TYPE_INT,
      params->newWidth,
      "height",
      G_TYPE_INT,
      params->newHeight,
      NULL);
  gst_caps_set_features(caps, 0, gst_caps_features_from_string("memory:NVMM"));
  g_object_set(G_OBJECT(capsfilter), "caps", caps, NULL);
  gst_caps_unref(caps);

  // Configure encoder.
  if (params->lossless) {
    g_print("RTSP: Configuring encoder for lossless encoding.\n");
    g_object_set(G_OBJECT(encoder), "rc-mode", 2, NULL);
    g_object_set(G_OBJECT(encoder), "qp", 0, NULL);
  } else if (params->bitrate > 0) {
    g_print("RTSP: Setting encoder bitrate to %d kbit/s.\n", params->bitrate);
    g_object_set(G_OBJECT(encoder), "bitrate", params->bitrate, NULL);
    g_object_set(G_OBJECT(encoder), "rc-mode", 1, NULL);
  }

  // Add all elements to the bin.
  gst_bin_add_many(
      GST_BIN(bin),
      source,
      demux,
      videoQueue,
      vparse,
      decoder,
      vidConvert,
      capsfilter,
      encoder,
      postParse,
      rtpPayVideo,
      audioQueue,
      aacparse,
      rtpPayAudio,
      NULL);

  if (!gst_element_link(source, demux)) {
    g_printerr("RTSP: Failed to link source to demux.\n");
    return NULL;
  }

  if (!gst_element_link_many(videoQueue, vparse, decoder, vidConvert, capsfilter, encoder, postParse, NULL)) {
    g_printerr("RTSP: Failed to link video branch elements.\n");
    return NULL;
  }
  if (!gst_element_link(postParse, rtpPayVideo)) {
    g_printerr("RTSP: Failed to link postParse to rtph265pay.\n");
    return NULL;
  }
  if (!gst_element_link_many(audioQueue, aacparse, rtpPayAudio, NULL)) {
    g_printerr("RTSP: Failed to link audio branch elements.\n");
    return NULL;
  }

  // Connect demuxer pad-added signal.
  DemuxData* demuxData = new DemuxData();
  demuxData->videoQueue = videoQueue;
  demuxData->audioQueue = audioQueue;
  g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), demuxData);
  // (In production, free demuxData when no longer needed.)

  // Create ghost pads for the RTP payloaders.
  GstPad* pay0_src = gst_element_get_static_pad(rtpPayVideo, "src");
  GstPad* ghost_video = gst_ghost_pad_new("pay0", pay0_src);
  gst_element_add_pad(bin, ghost_video);
  gst_object_unref(pay0_src);

  GstPad* pay1_src = gst_element_get_static_pad(rtpPayAudio, "src");
  GstPad* ghost_audio = gst_ghost_pad_new("pay1", pay1_src);
  gst_element_add_pad(bin, ghost_audio);
  gst_object_unref(pay1_src);

  return bin;
}

// -----------------------------------------------------------------
// RTSP Media Factory subclass: MyRTSPMediaFactory
// -----------------------------------------------------------------
extern "C" {

#define MY_TYPE_RTSP_MEDIA_FACTORY (my_rtsp_media_factory_get_type())
G_DECLARE_FINAL_TYPE(MyRTSPMediaFactory, my_rtsp_media_factory, MY, RTSP_MEDIA_FACTORY, GstRTSPMediaFactory)

struct _MyRTSPMediaFactory {
  GstRTSPMediaFactory parent;
  RTSPParams params; // Parameters for RTSP pipeline.
};

G_DEFINE_TYPE(MyRTSPMediaFactory, my_rtsp_media_factory, GST_TYPE_RTSP_MEDIA_FACTORY)

static GstElement* my_rtsp_media_factory_create_element(GstRTSPMediaFactory* factory, const GstRTSPUrl* url) {
  MyRTSPMediaFactory* myfactory = MY_RTSP_MEDIA_FACTORY(factory);
  // Create the RTSP pipeline bin using our parameters.
  return create_rtsp_pipeline(factory, &myfactory->params);
}

static void my_rtsp_media_factory_class_init(MyRTSPMediaFactoryClass* klass) {
  GstRTSPMediaFactoryClass* factory_class = GST_RTSP_MEDIA_FACTORY_CLASS(klass);
  factory_class->create_element = my_rtsp_media_factory_create_element;
}

static void my_rtsp_media_factory_init(MyRTSPMediaFactory* factory) {
  // Initialize default parameters.
  factory->params.inputFilename[0] = '\0';
  factory->params.newWidth = 0;
  factory->params.newHeight = 0;
  factory->params.bitrate = 0;
  factory->params.lossless = false;
}

} // end extern "C"

// -----------------------------------------------------------------
// Main: choose file-output or RTSP mode
// -----------------------------------------------------------------
int main(int argc, char* argv[]) {
  gst_init(&argc, &argv);

  if (argc < 3) {
    g_printerr(
        "Usage: %s <input file> <output file> [--rtsp] [--maxframes=<number>] [--bitrate=<value>] [--lossless]\n",
        argv[0]);
    return -1;
  }
  std::string inputFilename = argv[1];
  std::string outputFilename = argv[2];
  bool rtspMode = false;
  gint maxFrames = 0;
  gint bitrate = 0;
  bool lossless = false;
  for (int i = 3; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--rtsp")
      rtspMode = true;
    else if (arg.find("--maxframes=") == 0)
      maxFrames = std::stoi(arg.substr(strlen("--maxframes=")));
    else if (arg.find("--bitrate=") == 0)
      bitrate = std::stoi(arg.substr(strlen("--bitrate=")));
    else if (arg == "--lossless")
      lossless = true;
  }

  // Query video dimensions.
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
    // RTSP Mode.
    MyRTSPMediaFactory* factory = static_cast<MyRTSPMediaFactory*>(g_object_new(MY_TYPE_RTSP_MEDIA_FACTORY, NULL));
    strncpy(factory->params.inputFilename, inputFilename.c_str(), sizeof(factory->params.inputFilename) - 1);
    factory->params.inputFilename[sizeof(factory->params.inputFilename) - 1] = '\0';
    factory->params.newWidth = newWidth;
    factory->params.newHeight = newHeight;
    factory->params.bitrate = bitrate;
    factory->params.lossless = lossless;

    GstRTSPServer* server = gst_rtsp_server_new();
    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server);
    gst_rtsp_mount_points_add_factory(mounts, "/test", GST_RTSP_MEDIA_FACTORY(factory));
    g_object_unref(mounts);
    gst_rtsp_server_attach(server, NULL);
    g_print("RTSP server is live at rtsp://127.0.0.1:8554/test\n");
    GMainLoop* rtspLoop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(rtspLoop);
    g_main_loop_unref(rtspLoop);
    return 0;
  } else {
    // File-output mode.
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    GstElement* pipeline = gst_pipeline_new("deepstream-pipeline");

    // Create source and demuxer.
    GstElement* source = gst_element_factory_make("filesrc", "source");
    GstElement* demux = gst_element_factory_make("qtdemux", "demux");

    GstElement* audio_source = gst_element_factory_make("filesrc", "audio_source");
    GstElement* audio_demux = gst_element_factory_make("qtdemux", "audio_demux");

    // Video branch.
    GstElement* videoQueue = gst_element_factory_make("queue", "videoQueue");
    GstElement* h265parse = gst_element_factory_make("h265parse", "h265parse");
    GstElement* decoder = gst_element_factory_make("avdec_h265", "decoder");
    GstElement* vidConvert = gst_element_factory_make("nvvideoconvert", "vidConvert");
    GstElement* capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
    GstElement* encoder = gst_element_factory_make("nvv4l2h265enc", "encoder");
    GstElement* postParse = gst_element_factory_make("h265parse", "postParse");

    // Audio branch.
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

    g_object_set(G_OBJECT(source), "location", inputFilename.c_str(), NULL);
    g_object_set(G_OBJECT(audio_source), "location", inputFilename.c_str(), NULL);
    g_object_set(G_OBJECT(sink), "location", outputFilename.c_str(), NULL);

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

    if (lossless) {
      g_print("Configuring encoder for lossless encoding.\n");
      g_object_set(G_OBJECT(encoder), "rc-mode", 2, NULL);
      g_object_set(G_OBJECT(encoder), "qp", 0, NULL);
    } else if (bitrate > 0) {
      g_print("Setting encoder bitrate to %d kbit/s.\n", bitrate);
      g_object_set(G_OBJECT(encoder), "bitrate", bitrate, NULL);
      g_object_set(G_OBJECT(encoder), "rc-mode", 1, NULL);
    }

    gst_bin_add_many(
        GST_BIN(pipeline),
        source,
        demux,
        audio_source,
        audio_demux,
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

    if (!gst_element_link(source, demux)) {
      g_printerr("Failed to link source to demux.\n");
      return -1;
    }
    if (!gst_element_link(audio_source, audio_demux)) {
      g_printerr("Failed to link source to audio_demux.\n");
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

    // DemuxData demuxData = {videoQueue, audioQueue};
    // g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), &demuxData);

    DemuxData videoDemuxData = {videoQueue, nullptr};
    g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), &videoDemuxData);

    DemuxData audioDemuxData = {nullptr, audioQueue};
    g_signal_connect(audio_demux, "pad-added", G_CALLBACK(on_pad_added), &audioDemuxData);

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

    AppData appData;
    appData.loop = loop;
    appData.pipeline = pipeline;
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, bus_call, &appData);
    gst_object_unref(bus);

    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    g_print("Running file-output pipeline...\n");
    g_main_loop_run(loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    return 0;
  }
}
