#include <glib.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <atomic>
#include <iostream>

/* RTSP Server context */
typedef struct {
  GMainLoop* loop;
  GstRTSPServer* server;
  GstRTSPMountPoints* mount_points;
  GstRTSPMediaFactory* factory;
  gchar* rtsp_port;
  gchar* mount_point;
} RTSPServerContext;

/* Global variables */
static GMainLoop* g_main_loop = NULL;
static GstElement* g_pipeline = NULL;
static std::atomic<bool> g_has_audio(false);

/* Function declarations */
static void setup_rtsp_server(RTSPServerContext* rtsp_ctx);
static gboolean bus_call(GstBus* bus, GstMessage* msg, gpointer data);
static GstElement* create_deepstream_pipeline(const char* input_file);
static void check_for_audio_stream(const char* input_file);
static void print_runtime_commands();
static void on_demux_pad_added_for_detection(GstElement* element, GstPad* pad, gpointer data);
static void on_demux_pad_added_for_pipeline(GstElement* element, GstPad* pad, gpointer data);
static void on_rtsp_media_configure(GstRTSPMediaFactory* factory, GstRTSPMedia* media, gpointer user_data);

/* Callback for pad-added signal during audio detection */
static void on_demux_pad_added_for_detection(GstElement* element, GstPad* pad, gpointer data) {
  GstCaps* caps = gst_pad_get_current_caps(pad);
  GstStructure* str = gst_caps_get_structure(caps, 0);
  const gchar* name = gst_structure_get_name(str);

  if (g_str_has_prefix(name, "audio")) {
    g_has_audio = true;
    g_print("Found audio stream in the input file\n");
  }

  gst_caps_unref(caps);
}

/* Callback for pad-added signal in the main pipeline */
static void on_demux_pad_added_for_pipeline(GstElement* element, GstPad* pad, gpointer data) {
  GstElement* pipeline = GST_ELEMENT(data);
  GstCaps* caps = gst_pad_get_current_caps(pad);
  GstStructure* str = gst_caps_get_structure(caps, 0);
  const gchar* name = gst_structure_get_name(str);

  if (g_str_has_prefix(name, "video/x-h265")) {
    /* Link the demuxer's video pad to h265parse */
    GstElement* h265parse = gst_bin_get_by_name(GST_BIN(pipeline), "h265-parser");
    GstPad* sinkpad = gst_element_get_static_pad(h265parse, "sink");

    if (GST_PAD_LINK_OK != gst_pad_link(pad, sinkpad)) {
      g_printerr("Failed to link demuxer to h265parse\n");
    } else {
      g_print("Linked demuxer video pad to h265parse\n");
    }

    gst_object_unref(sinkpad);
    gst_object_unref(h265parse);
  } else if (g_has_audio && g_str_has_prefix(name, "audio/")) {
    /* Link the demuxer's audio pad to audio queue */
    GstElement* audio_queue = gst_bin_get_by_name(GST_BIN(pipeline), "audio-queue");
    GstPad* sinkpad = gst_element_get_static_pad(audio_queue, "sink");

    if (GST_PAD_LINK_OK != gst_pad_link(pad, sinkpad)) {
      g_printerr("Failed to link demuxer to audio queue\n");
    } else {
      g_print("Linked demuxer audio pad to audio queue\n");
    }

    gst_object_unref(sinkpad);
    gst_object_unref(audio_queue);

    /* Link audio queue to audioparse */
    GstElement* audioparse = gst_bin_get_by_name(GST_BIN(pipeline), "aac-parser");
    if (!gst_element_link(audio_queue, audioparse)) {
      g_printerr("Failed to link audio queue to audioparse\n");
    }

    gst_object_unref(audioparse);
  }

  gst_caps_unref(caps);
}

/* Callback for RTSP media configure */
static void on_rtsp_media_configure(GstRTSPMediaFactory* factory, GstRTSPMedia* media, gpointer user_data) {
  GstElement* element = gst_rtsp_media_get_element(media);

  /* Configure video source if present */
  GstElement* video_src = gst_bin_get_by_name_recurse_up(GST_BIN(element), "video-src");
  if (video_src) {
    g_object_set(G_OBJECT(video_src), "is-live", TRUE, "do-timestamp", TRUE, NULL);
    gst_object_unref(video_src);
  }

  /* Configure audio source if present */
  if (g_has_audio) {
    GstElement* audio_src = gst_bin_get_by_name_recurse_up(GST_BIN(element), "audio-src");
    if (audio_src) {
      g_object_set(G_OBJECT(audio_src), "is-live", TRUE, "do-timestamp", TRUE, NULL);
      gst_object_unref(audio_src);
    }
  }

  gst_object_unref(element);
}

int main(int argc, char* argv[]) {
  GstBus* bus = NULL;
  GstStateChangeReturn ret;
  guint bus_watch_id;

  /* Check input arguments */
  if (argc != 2) {
    g_printerr("Usage: %s <H265 MP4 file path>\n", argv[0]);
    return -1;
  }

  const char* input_file = argv[1];

  /* Initialize GStreamer */
  gst_init(&argc, &argv);

  /* Check if input file has audio */
  check_for_audio_stream(input_file);

  /* Create main loop */
  g_main_loop = g_main_loop_new(NULL, FALSE);

  /* Create DeepStream pipeline */
  g_pipeline = create_deepstream_pipeline(input_file);
  if (!g_pipeline) {
    g_printerr("Failed to create pipeline\n");
    return -1;
  }

  /* Setup RTSP server */
  RTSPServerContext rtsp_ctx = {
      .loop = g_main_loop,
      .server = NULL,
      .mount_points = NULL,
      .factory = NULL,
      .rtsp_port = (gchar*)"8554",
      .mount_point = (gchar*)"/ds-test"};

  setup_rtsp_server(&rtsp_ctx);

  /* Set up the pipeline */
  /* Add message handler */
  bus = gst_element_get_bus(g_pipeline);
  bus_watch_id = gst_bus_add_watch(bus, bus_call, g_main_loop);
  gst_object_unref(bus);

  /* Start playing */
  g_print("Starting pipeline...\n");
  ret = gst_element_set_state(g_pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Unable to set the pipeline to the playing state\n");
    gst_object_unref(g_pipeline);
    return -1;
  }

  print_runtime_commands();

  /* Start the main loop */
  g_print("RTSP stream ready at rtsp://localhost:%s%s\n", rtsp_ctx.rtsp_port, rtsp_ctx.mount_point);
  g_print("Running...\n");
  g_main_loop_run(g_main_loop);

  /* Clean up */
  g_print("Deleting pipeline\n");
  gst_element_set_state(g_pipeline, GST_STATE_NULL);
  gst_object_unref(g_pipeline);
  g_source_remove(bus_watch_id);
  g_main_loop_unref(g_main_loop);

  return 0;
}

/* Setup RTSP server with mount points */
static void setup_rtsp_server(RTSPServerContext* rtsp_ctx) {
  /* Create RTSP server */
  rtsp_ctx->server = gst_rtsp_server_new();
  g_object_set(rtsp_ctx->server, "service", rtsp_ctx->rtsp_port, NULL);

  /* Get mount points for the server */
  rtsp_ctx->mount_points = gst_rtsp_server_get_mount_points(rtsp_ctx->server);

  /* Create a media factory for the mount point */
  rtsp_ctx->factory = gst_rtsp_media_factory_new();

  /* Generate the pipeline description programmatically */
  GString* pipeline_str = g_string_new("( ");

  /* Add video pipeline */
  g_string_append(
      pipeline_str,
      "appsrc name=video-src is-live=true format=time ! "
      "video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! "
      "nvvidconv ! "
      "nvv4l2h264enc bitrate=4000000 ! "
      "h264parse ! "
      "rtph264pay name=pay0 pt=96");

  /* Add audio pipeline if needed */
  if (g_has_audio) {
    g_string_append(
        pipeline_str,
        " appsrc name=audio-src is-live=true format=time ! "
        "audio/x-raw,format=S16LE,rate=44100,channels=2 ! "
        "audioconvert ! "
        "audioresample ! "
        "opusenc bitrate=128000 ! "
        "rtpopuspay name=pay1 pt=97");
  }

  g_string_append(pipeline_str, " )");

  /* Set the pipeline */
  gst_rtsp_media_factory_set_launch(rtsp_ctx->factory, pipeline_str->str);
  g_print("RTSP Pipeline: %s\n", pipeline_str->str);
  g_string_free(pipeline_str, TRUE);

  /* Set shared property to true to allow multiple clients */
  gst_rtsp_media_factory_set_shared(rtsp_ctx->factory, TRUE);

  /* Configure RTSP factory properties */
  gst_rtsp_media_factory_set_transport_mode(rtsp_ctx->factory, GST_RTSP_TRANSPORT_MODE_PLAY);

  /* Set supported transport protocols */
  gst_rtsp_media_factory_set_protocols(
      rtsp_ctx->factory,
      (GstRTSPLowerTrans)(GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_UDP_MCAST | GST_RTSP_LOWER_TRANS_TCP));

  /* Connect to media-configure signal */
  g_signal_connect(rtsp_ctx->factory, "media-configure", G_CALLBACK(on_rtsp_media_configure), NULL);

  /* Attach the factory to the mount point */
  gst_rtsp_mount_points_add_factory(rtsp_ctx->mount_points, rtsp_ctx->mount_point, rtsp_ctx->factory);
  g_object_unref(rtsp_ctx->mount_points);

  /* Attach the server to the default main context */
  gst_rtsp_server_attach(rtsp_ctx->server, NULL);
}

/* Handler for GstBus messages */
static gboolean bus_call(GstBus* bus, GstMessage* msg, gpointer data) {
  GMainLoop* loop = (GMainLoop*)data;

  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
      g_print("End of stream\n");
      g_main_loop_quit(loop);
      break;

    case GST_MESSAGE_ERROR: {
      gchar* debug;
      GError* error;

      gst_message_parse_error(msg, &error, &debug);
      g_printerr("ERROR from element %s: %s\n", GST_OBJECT_NAME(msg->src), error->message);

      if (debug)
        g_printerr("Error details: %s\n", debug);

      g_free(debug);
      g_error_free(error);

      g_main_loop_quit(loop);
      break;
    }

    default:
      break;
  }

  return TRUE;
}

/* Check if the input file has an audio stream */
static void check_for_audio_stream(const char* input_file) {
  GstElement* pipeline = NULL;
  GstElement* source = NULL;
  GstElement* demuxer = NULL;
  GstBus* bus = NULL;

  pipeline = gst_pipeline_new("check-audio-pipeline");
  source = gst_element_factory_make("filesrc", "file-source");
  demuxer = gst_element_factory_make("qtdemux", "qt-demuxer");

  if (!pipeline || !source || !demuxer) {
    g_printerr("Failed to create elements for audio check\n");
    if (pipeline)
      gst_object_unref(pipeline);
    return;
  }

  g_object_set(G_OBJECT(source), "location", input_file, NULL);

  gst_bin_add_many(GST_BIN(pipeline), source, demuxer, NULL);
  gst_element_link(source, demuxer);

  /* Connect to the "pad-added" signal of the demuxer */
  g_signal_connect(demuxer, "pad-added", G_CALLBACK(on_demux_pad_added_for_detection), NULL);

  bus = gst_element_get_bus(pipeline);

  /* Start the pipeline */
  gst_element_set_state(pipeline, GST_STATE_PLAYING);

  /* Wait for 3 seconds max to detect audio streams */
  GstMessage* msg =
      gst_bus_timed_pop_filtered(bus, 3 * GST_SECOND, (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

  if (msg) {
    gst_message_unref(msg);
  }

  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(bus);
  gst_object_unref(pipeline);

  if (g_has_audio) {
    g_print("Will stream audio and video\n");
  } else {
    g_print("No audio stream detected, will stream video only\n");
  }
}

/* Create the DeepStream GStreamer pipeline */
static GstElement* create_deepstream_pipeline(const char* input_file) {
  GstElement* pipeline = NULL;
  GstElement* source = NULL;
  GstElement* qtdemux = NULL;
  GstElement* h265parse = NULL;
  GstElement* decoder = NULL;
  GstElement* nvvidconv = NULL;
  GstElement* nvstreammux = NULL;
  GstElement* nvosd = NULL;
  GstElement* nvvidconv_postosd = NULL;
  GstElement* caps_filter = NULL;
  GstElement* encoder = NULL;
  GstElement* h264parse = NULL;
  GstElement* video_queue = NULL;
  GstElement* video_rtppay = NULL;
  GstElement* video_udpsink = NULL;

  /* Audio elements */
  GstElement* audio_queue = NULL;
  GstElement* audioparse = NULL;
  GstElement* audiodecoder = NULL;
  GstElement* audioconvert = NULL;
  GstElement* audioresample = NULL;
  GstElement* audioenc = NULL;
  GstElement* audio_rtppay = NULL;
  GstElement* audio_udpsink = NULL;

  GstCaps* caps = NULL;

  /* Create pipeline */
  pipeline = gst_pipeline_new("ds-rtsp-pipeline");

  /* Create common elements */
  source = gst_element_factory_make("filesrc", "file-source");
  qtdemux = gst_element_factory_make("qtdemux", "qtdemux");

  /* Create video elements */
  h265parse = gst_element_factory_make("h265parse", "h265-parser");
  decoder = gst_element_factory_make("nvv4l2decoder", "nvv4l2-decoder");
  nvvidconv = gst_element_factory_make("nvvideoconvert", "nvvideo-converter");
  nvstreammux = gst_element_factory_make("nvstreammux", "stream-muxer");
  nvosd = gst_element_factory_make("nvdsosd", "nv-onscreendisplay");
  nvvidconv_postosd = gst_element_factory_make("nvvideoconvert", "nvvideo-converter-postosd");
  caps_filter = gst_element_factory_make("capsfilter", "caps-filter");
  encoder = gst_element_factory_make("nvv4l2h264enc", "nvv4l2-h264-encoder");
  h264parse = gst_element_factory_make("h264parse", "h264-parser");
  video_queue = gst_element_factory_make("queue", "video-queue");
  video_rtppay = gst_element_factory_make("rtph264pay", "rtp-payer");
  video_udpsink = gst_element_factory_make("udpsink", "video-udpsink");

  if (!pipeline || !source || !qtdemux || !h265parse || !decoder || !nvvidconv || !nvstreammux || !nvosd ||
      !nvvidconv_postosd || !caps_filter || !encoder || !h264parse || !video_queue || !video_rtppay || !video_udpsink) {
    g_printerr("One or more video elements could not be created. Exiting.\n");
    return NULL;
  }

  /* Create audio elements if needed */
  if (g_has_audio) {
    audio_queue = gst_element_factory_make("queue", "audio-queue");
    audioparse = gst_element_factory_make("aacparse", "aac-parser"); // Assuming AAC audio
    audiodecoder = gst_element_factory_make("avdec_aac", "aac-decoder");
    audioconvert = gst_element_factory_make("audioconvert", "audio-converter");
    audioresample = gst_element_factory_make("audioresample", "audio-resampler");
    audioenc = gst_element_factory_make("opusenc", "opus-encoder");
    audio_rtppay = gst_element_factory_make("rtpopuspay", "rtp-opus-payer");
    audio_udpsink = gst_element_factory_make("udpsink", "audio-udpsink");

    if (!audio_queue || !audioparse || !audiodecoder || !audioconvert || !audioresample || !audioenc || !audio_rtppay ||
        !audio_udpsink) {
      g_printerr("One or more audio elements could not be created. Falling back to video only.\n");
      g_has_audio = false;
    }
  }

  /* Set element properties */
  g_object_set(G_OBJECT(source), "location", input_file, NULL);
  g_object_set(
      G_OBJECT(nvstreammux), "width", 1920, "height", 1080, "batch-size", 1, "batched-push-timeout", 40000, NULL);

  caps = gst_caps_from_string("video/x-raw(memory:NVMM), format=NV12");
  g_object_set(G_OBJECT(caps_filter), "caps", caps, NULL);
  gst_caps_unref(caps);

  g_object_set(G_OBJECT(encoder), "bitrate", 4000000, NULL);
  g_object_set(G_OBJECT(video_rtppay), "pt", 96, NULL);
  g_object_set(G_OBJECT(video_udpsink), "host", "127.0.0.1", "port", 5400, "sync", TRUE, "async", FALSE, NULL);

  if (g_has_audio) {
    g_object_set(G_OBJECT(audioenc), "bitrate", 128000, NULL);
    g_object_set(G_OBJECT(audio_rtppay), "pt", 97, NULL);
    g_object_set(G_OBJECT(audio_udpsink), "host", "127.0.0.1", "port", 5401, "sync", TRUE, "async", FALSE, NULL);
  }

  /* Add elements to the pipeline */
  gst_bin_add_many(GST_BIN(pipeline), source, qtdemux, NULL);

  /* We can't link qtdemux with other elements yet as its pads are created dynamically */
  if (!gst_element_link(source, qtdemux)) {
    g_printerr("Source and demuxer could not be linked. Exiting.\n");
    gst_object_unref(pipeline);
    return NULL;
  }

  /* Add video elements to the pipeline */
  gst_bin_add_many(
      GST_BIN(pipeline),
      h265parse,
      decoder,
      nvvidconv,
      nvstreammux,
      nvosd,
      nvvidconv_postosd,
      caps_filter,
      encoder,
      h264parse,
      video_queue,
      video_rtppay,
      video_udpsink,
      NULL);

  /* Add audio elements to the pipeline if needed */
  if (g_has_audio) {
    gst_bin_add_many(
        GST_BIN(pipeline),
        audio_queue,
        audioparse,
        audiodecoder,
        audioconvert,
        audioresample,
        audioenc,
        audio_rtppay,
        audio_udpsink,
        NULL);
  }

  /* Link video elements after demuxer */
  if (!gst_element_link_many(h265parse, decoder, nvvidconv, NULL)) {
    g_printerr("Video elements after demuxer could not be linked. Exiting.\n");
    gst_object_unref(pipeline);
    return NULL;
  }

  /* Create and link pad for nvstreammux */
  GstPad *sinkpad, *srcpad;
  GstPadTemplate* pad_template;

  pad_template = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(nvstreammux), "sink_%u");
  sinkpad = gst_element_request_pad(nvstreammux, pad_template, "sink_0", NULL);

  srcpad = gst_element_get_static_pad(nvvidconv, "src");
  if (gst_pad_link(srcpad, sinkpad) != GST_PAD_LINK_OK) {
    g_printerr("Failed to link converter to stream muxer. Exiting.\n");
    gst_object_unref(pipeline);
    return NULL;
  }
  gst_object_unref(srcpad);
  gst_object_unref(sinkpad);

  /* Link the remaining video elements */
  if (!gst_element_link_many(
          nvstreammux,
          nvosd,
          nvvidconv_postosd,
          caps_filter,
          encoder,
          h264parse,
          video_queue,
          video_rtppay,
          video_udpsink,
          NULL)) {
    g_printerr("Video elements after stream muxer could not be linked. Exiting.\n");
    gst_object_unref(pipeline);
    return NULL;
  }

  /* Link audio elements if needed */
  if (g_has_audio) {
    if (!gst_element_link_many(
            audioparse, audiodecoder, audioconvert, audioresample, audioenc, audio_rtppay, audio_udpsink, NULL)) {
      g_printerr("Audio elements could not be linked. Falling back to video only.\n");
      g_has_audio = false;
    }
  }

  /* Connect to the "pad-added" signal of qtdemux */
  g_signal_connect(qtdemux, "pad-added", G_CALLBACK(on_demux_pad_added_for_pipeline), pipeline);

  return pipeline;
}

/* Print runtime commands */
static void print_runtime_commands() {
  g_print(
      "\nRuntime commands:\n"
      "* Press Ctrl+C to stop the pipeline\n\n");
}
