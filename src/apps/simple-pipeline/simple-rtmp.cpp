#include <gst/gst.h>
#include <cstring>
#include <iostream>
#include <string>

// Structure to hold our application data
typedef struct {
  GstElement* pipeline;
  GstElement* videosrc; // Video file source
  GstElement* audiosrc; // Audio file source
  GstElement* videodemux; // Video demuxer (if needed)
  GstElement* audiodemux; // Audio demuxer (if needed)
  GstElement* h264parse; // H.264 parser
  GstElement* aacparse; // AAC parser
  GstElement* videodec; // Video decoder (optional)
  GstElement* audiodec; // Audio decoder (optional)
  GstElement* videoconv; // Video converter (if needed)
  GstElement* audioconv; // Audio converter (if needed)
  GstElement* flvmux; // FLV muxer
  GstElement* rtmpsink; // RTMP sink
  bool video_linked;
  bool audio_linked;
  unsigned int video_frame_count;
  unsigned int audio_frame_count;
} AppData;

// Function declarations
static GstPadProbeReturn debug_probe_callback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
static GstPadProbeReturn video_frame_probe_callback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
static GstPadProbeReturn audio_frame_probe_callback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
static void on_video_pad_added(GstElement* element, GstPad* pad, gpointer user_data);
static void on_audio_pad_added(GstElement* element, GstPad* pad, gpointer user_data);
static void on_debug_pad_added(GstElement* element, GstPad* pad, gpointer user_data);
static void add_debug_probes_to_element(GstElement* element);
static gboolean bus_call(GstBus* bus, GstMessage* msg, gpointer data);

// Debug probe function to print information about buffers on each pad
static GstPadProbeReturn debug_probe_callback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    GstElement* element = GST_PAD_PARENT(pad);
    const gchar* element_name = GST_ELEMENT_NAME(element);
    const gchar* pad_name = GST_PAD_NAME(pad);

    // Get pad direction
    const char* direction;
    switch (gst_pad_get_direction(pad)) {
      case GST_PAD_SRC:
        direction = "SRC";
        break;
      case GST_PAD_SINK:
        direction = "SINK";
        break;
      default:
        direction = "UNKNOWN";
        break;
    }

    // Get the caps of the pad if available
    GstCaps* caps = gst_pad_get_current_caps(pad);
    gchar* caps_str = caps ? gst_caps_to_string(caps) : g_strdup("NONE");

    // Get buffer timestamp and size
    GstClockTime timestamp = GST_BUFFER_PTS(buffer);
    gsize size = gst_buffer_get_size(buffer);

    g_print(
        "DEBUG: Element [%s] Pad [%s:%s] Caps [%s] Buffer: size=%" G_GSIZE_FORMAT " pts=%" GST_TIME_FORMAT "\n",
        element_name,
        pad_name,
        direction,
        caps_str,
        size,
        GST_TIME_ARGS(timestamp));

    g_free(caps_str);
    if (caps) {
      gst_caps_unref(caps);
    }
  }

  // Let the buffer pass through
  return GST_PAD_PROBE_OK;
}

// Probe function to count video frames
static GstPadProbeReturn video_frame_probe_callback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
  AppData* data = (AppData*)user_data;

  data->video_frame_count++;
  if (data->video_frame_count % 10 == 0) {
    g_print("Sent %d video frames to RTMP server\n", data->video_frame_count);
  }

  // Let the buffer pass through
  return GST_PAD_PROBE_OK;
}

// Probe function to count audio frames
static GstPadProbeReturn audio_frame_probe_callback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
  AppData* data = (AppData*)user_data;

  data->audio_frame_count++;
  if (data->audio_frame_count % 10 == 0) {
    g_print("Sent %d audio frames to RTMP server\n", data->audio_frame_count);
  }

  // Let the buffer pass through
  return GST_PAD_PROBE_OK;
}

// Function to add debug probes to all pads of an element
static void add_debug_probes_to_element(GstElement* element) {
  const gchar* element_name = GST_ELEMENT_NAME(element);
  g_print("Adding debug probes to all pads in element: %s\n", element_name);

  // Get iterator for src pads
  GstIterator* pad_iter = gst_element_iterate_src_pads(element);
  if (pad_iter) {
    GValue item = G_VALUE_INIT;
    gboolean done = FALSE;

    while (!done) {
      switch (gst_iterator_next(pad_iter, &item)) {
        case GST_ITERATOR_OK: {
          GstPad* pad = GST_PAD(g_value_get_object(&item));
          const gchar* pad_name = GST_PAD_NAME(pad);
          g_print("  Adding probe to source pad: %s\n", pad_name);
          gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, (GstPadProbeCallback)debug_probe_callback, NULL, NULL);
          g_value_reset(&item);
          break;
        }
        case GST_ITERATOR_RESYNC:
          gst_iterator_resync(pad_iter);
          break;
        case GST_ITERATOR_ERROR:
        case GST_ITERATOR_DONE:
          done = TRUE;
          break;
      }
    }
    g_value_unset(&item);
    gst_iterator_free(pad_iter);
  }

  // Get iterator for sink pads
  pad_iter = gst_element_iterate_sink_pads(element);
  if (pad_iter) {
    GValue item = G_VALUE_INIT;
    gboolean done = FALSE;

    while (!done) {
      switch (gst_iterator_next(pad_iter, &item)) {
        case GST_ITERATOR_OK: {
          GstPad* pad = GST_PAD(g_value_get_object(&item));
          const gchar* pad_name = GST_PAD_NAME(pad);
          g_print("  Adding probe to sink pad: %s\n", pad_name);
          gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, (GstPadProbeCallback)debug_probe_callback, NULL, NULL);
          g_value_reset(&item);
          break;
        }
        case GST_ITERATOR_RESYNC:
          gst_iterator_resync(pad_iter);
          break;
        case GST_ITERATOR_ERROR:
        case GST_ITERATOR_DONE:
          done = TRUE;
          break;
      }
    }
    g_value_unset(&item);
    gst_iterator_free(pad_iter);
  }
}

// Function to handle pad-added signals from video demuxer
static void on_video_pad_added(GstElement* element, GstPad* pad, gpointer user_data) {
  AppData* data = (AppData*)user_data;
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps)
    return;

  GstStructure* structure = gst_caps_get_structure(caps, 0);
  const gchar* name = gst_structure_get_name(structure);

  if (g_str_has_prefix(name, "video/x-h264")) {
    GstPad* sinkpad = gst_element_get_static_pad(data->h264parse, "sink");
    if (GST_PAD_LINK_FAILED(gst_pad_link(pad, sinkpad))) {
      g_printerr("Failed to link video demuxer to h264parse\n");
    } else {
      g_print("Linked video demuxer pad to h264parse\n");
      data->video_linked = true;
    }
    gst_object_unref(sinkpad);
  }

  gst_caps_unref(caps);
}

// Function to handle pad-added signals from audio demuxer
static void on_audio_pad_added(GstElement* element, GstPad* pad, gpointer user_data) {
  AppData* data = (AppData*)user_data;
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps)
    return;

  GstStructure* structure = gst_caps_get_structure(caps, 0);
  const gchar* name = gst_structure_get_name(structure);

  if (g_str_has_prefix(name, "audio/mpeg")) {
    GstPad* sinkpad = gst_element_get_static_pad(data->aacparse, "sink");
    if (GST_PAD_LINK_FAILED(gst_pad_link(pad, sinkpad))) {
      g_printerr("Failed to link audio demuxer to aacparse\n");
    } else {
      g_print("Linked audio demuxer pad to aacparse\n");
      data->audio_linked = true;
    }
    gst_object_unref(sinkpad);
  }

  gst_caps_unref(caps);
}

// Function to handle dynamic pad creation and add debug probes
static void on_debug_pad_added(GstElement* element, GstPad* pad, gpointer user_data) {
  g_print("Adding debug probe to dynamic pad: %s\n", GST_PAD_NAME(pad));
  gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, (GstPadProbeCallback)debug_probe_callback, NULL, NULL);
}

// Function to handle both audio and video pads from a single demuxer
static void on_combined_demuxer_pad_added(GstElement* element, GstPad* pad, gpointer user_data) {
  AppData* data = (AppData*)user_data;
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps)
    return;

  GstStructure* structure = gst_caps_get_structure(caps, 0);
  const gchar* name = gst_structure_get_name(structure);
  const gchar* pad_name = GST_PAD_NAME(pad);

  g_print("Found stream: %s (pad: %s)\n", name, pad_name);

  if (g_str_has_prefix(name, "video/x-h264")) {
    GstPad* sinkpad = gst_element_get_static_pad(data->h264parse, "sink");
    if (GST_PAD_LINK_FAILED(gst_pad_link(pad, sinkpad))) {
      g_printerr("Failed to link video demuxer to h264parse\n");
    } else {
      g_print("Linked video demuxer pad to h264parse\n");
      data->video_linked = true;
    }
    gst_object_unref(sinkpad);
  } else if (g_str_has_prefix(name, "audio/mpeg")) {
    GstPad* sinkpad = gst_element_get_static_pad(data->aacparse, "sink");
    if (GST_PAD_LINK_FAILED(gst_pad_link(pad, sinkpad))) {
      g_printerr("Failed to link audio demuxer to aacparse\n");
    } else {
      g_print("Linked audio demuxer pad to aacparse\n");
      data->audio_linked = true;
    }
    gst_object_unref(sinkpad);
  }

  // Add debug probe to this dynamic pad
  g_print("Adding debug probe to dynamic pad: %s\n", GST_PAD_NAME(pad));
  gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, (GstPadProbeCallback)debug_probe_callback, NULL, NULL);

  gst_caps_unref(caps);
}

// Function to check for errors in the bus
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
      g_printerr("Error: %s\n", error->message);
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

int main(int argc, char* argv[]) {
  if (argc != 4 && argc != 3) {
    g_printerr("Usage: %s <video-file> <audio-file> <rtmp-url>\n", argv[0]);
    g_printerr("   or: %s <combined-audio-video-file> <rtmp-url>\n", argv[0]);
    return -1;
  }

  // Initialize GStreamer
  gst_init(&argc, &argv);

  AppData data;
  data.video_linked = false;
  data.audio_linked = false;
  data.video_frame_count = 0;
  data.audio_frame_count = 0;

  // Create the main loop
  GMainLoop* loop = g_main_loop_new(NULL, FALSE);

  // Create the pipeline
  data.pipeline = gst_pipeline_new("rtmp-streamer");

  // Check if we're using a single file for both audio and video
  bool single_file_mode = (argc == 3);
  const gchar* video_file = argv[1];
  const gchar* audio_file = single_file_mode ? argv[1] : argv[2];
  const gchar* rtmp_url = single_file_mode ? argv[2] : argv[3];

  // Create core elements
  data.flvmux = gst_element_factory_make("flvmux", "flv-muxer");
  data.rtmpsink = gst_element_factory_make("rtmpsink", "rtmp-sink");
  data.h264parse = gst_element_factory_make("h264parse", "h264-parser");
  data.aacparse = gst_element_factory_make("aacparse", "aac-parser");

  // Set up elements based on whether we're using a single file or separate files
  if (single_file_mode &&
      (g_str_has_suffix(video_file, ".mp4") || g_str_has_suffix(video_file, ".mov") ||
       g_str_has_suffix(video_file, ".m4v"))) {
    // Single file mode with MP4/MOV container
    g_print("Using single file mode with combined audio/video from: %s\n", video_file);

    // Create single source and demuxer
    data.videosrc = gst_element_factory_make("filesrc", "media-source");
    data.videodemux = gst_element_factory_make("qtdemux", "media-demuxer");
    data.audiosrc = NULL;
    data.audiodemux = NULL;

    // Set properties
    g_object_set(G_OBJECT(data.videosrc), "location", video_file, NULL);
  } else {
    // Separate file mode or non-MP4 single file
    g_print("Using separate sources for audio and video\n");

    // Create video elements
    data.videosrc = gst_element_factory_make("filesrc", "video-source");
    data.h264parse = gst_element_factory_make("h264parse", "h264-parser");

    // Create audio elements
    data.audiosrc = gst_element_factory_make("filesrc", "audio-source");
    data.aacparse = gst_element_factory_make("aacparse", "aac-parser");

    // Add video-specific elements depending on the file type
    if (g_str_has_suffix(video_file, ".h264") || g_str_has_suffix(video_file, ".264")) {
      // Raw H.264 stream - no demuxer needed
      data.videodemux = NULL;
    } else if (
        g_str_has_suffix(video_file, ".mp4") || g_str_has_suffix(video_file, ".mov") ||
        g_str_has_suffix(video_file, ".m4v")) {
      // MP4/MOV container - need qtdemux
      data.videodemux = gst_element_factory_make("qtdemux", "video-demuxer");
    } else if (g_str_has_suffix(video_file, ".ts") || g_str_has_suffix(video_file, ".mts")) {
      // MPEG-TS container - need tsdemux
      data.videodemux = gst_element_factory_make("tsdemux", "video-demuxer");
    } else if (g_str_has_suffix(video_file, ".mkv") || g_str_has_suffix(video_file, ".webm")) {
      // Matroska container - need matroskademux
      data.videodemux = gst_element_factory_make("matroskademux", "video-demuxer");
    } else {
      g_printerr("Unsupported video file format: %s\n", video_file);
      return -1;
    }

    // Add audio-specific elements depending on the file type
    if (g_str_has_suffix(audio_file, ".aac")) {
      // Raw AAC stream - no demuxer needed
      data.audiodemux = NULL;
    } else if (
        g_str_has_suffix(audio_file, ".mp4") || g_str_has_suffix(audio_file, ".mov") ||
        g_str_has_suffix(audio_file, ".m4a")) {
      // MP4/MOV container - need qtdemux
      data.audiodemux = gst_element_factory_make("qtdemux", "audio-demuxer");
    } else if (g_str_has_suffix(audio_file, ".mp3")) {
      // MP3 file - need mpegaudioparse
      data.audiodemux = NULL;
      data.aacparse = gst_element_factory_make("mpegaudioparse", "mp3-parser");
      data.audiodec = gst_element_factory_make("mpg123audiodec", "mp3-decoder");
      data.audioconv = gst_element_factory_make("audioconvert", "audio-converter");
      data.audiodec = gst_element_factory_make("faac", "aac-encoder");
    } else {
      g_printerr("Unsupported audio file format: %s\n", audio_file);
      return -1;
    }

    // Set properties
    g_object_set(G_OBJECT(data.videosrc), "location", video_file, NULL);
    if (data.audiosrc) {
      g_object_set(G_OBJECT(data.audiosrc), "location", audio_file, NULL);
    }
  }

  g_object_set(G_OBJECT(data.rtmpsink), "location", rtmp_url, NULL);
  g_object_set(G_OBJECT(data.flvmux), "streamable", TRUE, NULL);

  // Add message handler
  GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(data.pipeline));
  guint bus_watch_id = gst_bus_add_watch(bus, bus_call, loop);
  gst_object_unref(bus);

  // Add elements to pipeline based on mode
  if (single_file_mode && data.videosrc && data.videodemux) {
    g_print("Building single-file pipeline...\n");

    // Add elements to pipeline
    gst_bin_add(GST_BIN(data.pipeline), data.videosrc);
    gst_bin_add(GST_BIN(data.pipeline), data.videodemux);
    gst_bin_add(GST_BIN(data.pipeline), data.h264parse);
    gst_bin_add(GST_BIN(data.pipeline), data.aacparse);
    gst_bin_add(GST_BIN(data.pipeline), data.flvmux);
    gst_bin_add(GST_BIN(data.pipeline), data.rtmpsink);

    // Link elements
    if (!gst_element_link(data.videosrc, data.videodemux)) {
      g_printerr("Failed to link source to demuxer in single-file mode\n");
      return -1;
    }

    // Link both parsers to flvmux
    if (!gst_element_link_pads(data.h264parse, "src", data.flvmux, "video")) {
      g_printerr("Failed to link h264parse to flvmux\n");
      return -1;
    }

    if (!gst_element_link_pads(data.aacparse, "src", data.flvmux, "audio")) {
      g_printerr("Failed to link aacparse to flvmux\n");
      return -1;
    }

    // Link flvmux to rtmpsink
    if (!gst_element_link(data.flvmux, data.rtmpsink)) {
      g_printerr("Failed to link flvmux to rtmpsink\n");
      return -1;
    }

    // Connect pad-added signal for combined demuxer
    g_signal_connect(data.videodemux, "pad-added", G_CALLBACK(on_combined_demuxer_pad_added), &data);
  } else {
    // Add all elements to the pipeline
    g_print("Building separate-file pipeline...\n");

    gst_bin_add(GST_BIN(data.pipeline), data.videosrc);
    if (data.audiosrc)
      gst_bin_add(GST_BIN(data.pipeline), data.audiosrc);
    gst_bin_add(GST_BIN(data.pipeline), data.h264parse);
    gst_bin_add(GST_BIN(data.pipeline), data.aacparse);
    gst_bin_add(GST_BIN(data.pipeline), data.flvmux);
    gst_bin_add(GST_BIN(data.pipeline), data.rtmpsink);

    // Add demuxers if needed
    if (data.videodemux) {
      gst_bin_add(GST_BIN(data.pipeline), data.videodemux);
    }
    if (data.audiodemux) {
      gst_bin_add(GST_BIN(data.pipeline), data.audiodemux);
    }

    // Add additional elements for MP3 if needed
    if (g_str_has_suffix(audio_file, ".mp3") && data.audiodec && data.audioconv) {
      gst_bin_add(GST_BIN(data.pipeline), data.audiodec);
      gst_bin_add(GST_BIN(data.pipeline), data.audioconv);
    }

    // Link elements
    g_print("Linking pipeline elements...\n");

    // Link video elements
    if (data.videodemux) {
      // Link videosrc -> videodemux
      if (!gst_element_link(data.videosrc, data.videodemux)) {
        g_printerr("Failed to link video source to demuxer\n");
        return -1;
      }

      // Connect to pad-added signal to link dynamically created pads from demuxer
      g_signal_connect(data.videodemux, "pad-added", G_CALLBACK(on_video_pad_added), &data);
    } else {
      // Direct link for raw H264 streams
      if (!gst_element_link(data.videosrc, data.h264parse)) {
        g_printerr("Failed to link video source to h264parse\n");
        return -1;
      }
      data.video_linked = true;
    }

    // Link h264parse to flvmux
    if (!gst_element_link_pads(data.h264parse, "src", data.flvmux, "video")) {
      g_printerr("Failed to link h264parse to flvmux\n");
      return -1;
    }

    // Link audio elements
    if (data.audiodemux) {
      // Link audiosrc -> audiodemux
      if (!gst_element_link(data.audiosrc, data.audiodemux)) {
        g_printerr("Failed to link audio source to demuxer\n");
        return -1;
      }

      // Connect to pad-added signal to link dynamically created pads from demuxer
      g_signal_connect(data.audiodemux, "pad-added", G_CALLBACK(on_audio_pad_added), &data);
    } else if (g_str_has_suffix(audio_file, ".mp3") && data.audiodec && data.audioconv) {
      // MP3 path: audiosrc -> mpegaudioparse -> mpg123audiodec -> audioconvert -> faac -> flvmux
      if (!gst_element_link_many(data.audiosrc, data.aacparse, data.audiodec, data.audioconv, data.audiodec, NULL)) {
        g_printerr("Failed to link MP3 processing chain\n");
        return -1;
      }

      // Link encoder to flvmux
      if (!gst_element_link_pads(data.audiodec, "src", data.flvmux, "audio")) {
        g_printerr("Failed to link faac to flvmux\n");
        return -1;
      }

      data.audio_linked = true;
    } else if (data.audiosrc) {
      // Direct link for raw AAC streams
      if (!gst_element_link(data.audiosrc, data.aacparse)) {
        g_printerr("Failed to link audio source to aacparse\n");
        return -1;
      }

      // Link aacparse to flvmux
      if (!gst_element_link_pads(data.aacparse, "src", data.flvmux, "audio")) {
        g_printerr("Failed to link aacparse to flvmux\n");
        return -1;
      }

      data.audio_linked = true;
    }

    // Link flvmux to rtmpsink
    if (!gst_element_link(data.flvmux, data.rtmpsink)) {
      g_printerr("Failed to link flvmux to rtmpsink\n");
      return -1;
    }
  }

  // Add frame counting probes
  GstPad* video_pad = gst_element_get_static_pad(data.h264parse, "src");
  gst_pad_add_probe(video_pad, GST_PAD_PROBE_TYPE_BUFFER, (GstPadProbeCallback)video_frame_probe_callback, &data, NULL);
  gst_object_unref(video_pad);

  GstPad* audio_pad = gst_element_get_static_pad(data.aacparse, "src");
  gst_pad_add_probe(audio_pad, GST_PAD_PROBE_TYPE_BUFFER, (GstPadProbeCallback)audio_frame_probe_callback, &data, NULL);
  gst_object_unref(audio_pad);

  // Start playing
  g_print("Starting pipeline...\n");
  gst_element_set_state(data.pipeline, GST_STATE_PLAYING);

  if (single_file_mode) {
    g_print("Streaming combined audio/video from %s to %s\n", video_file, rtmp_url);
  } else {
    g_print("Streaming video: %s and audio: %s to %s\n", video_file, audio_file, rtmp_url);
  }

  // Add debug probes to all elements
  g_print("Adding debug probes to all elements...\n");
  add_debug_probes_to_element(data.videosrc);
  if (data.audiosrc)
    add_debug_probes_to_element(data.audiosrc);
  add_debug_probes_to_element(data.h264parse);
  add_debug_probes_to_element(data.aacparse);
  if (data.videodemux)
    add_debug_probes_to_element(data.videodemux);
  if (data.audiodemux)
    add_debug_probes_to_element(data.audiodemux);
  if (g_str_has_suffix(audio_file, ".mp3") && data.audiodec) {
    add_debug_probes_to_element(data.audiodec);
    add_debug_probes_to_element(data.audioconv);
  }
  add_debug_probes_to_element(data.flvmux);
  add_debug_probes_to_element(data.rtmpsink);

  // Connect pad-added signals for dynamic pads for debug probes
  if (data.videodemux && !single_file_mode) {
    g_signal_connect(data.videodemux, "pad-added", G_CALLBACK(on_debug_pad_added), NULL);
  }
  if (data.audiodemux) {
    g_signal_connect(data.audiodemux, "pad-added", G_CALLBACK(on_debug_pad_added), NULL);
  }

  // Run the main loop
  g_main_loop_run(loop);

  // Cleanup
  gst_element_set_state(data.pipeline, GST_STATE_NULL);
  gst_object_unref(GST_OBJECT(data.pipeline));
  g_source_remove(bus_watch_id);
  g_main_loop_unref(loop);

  return 0;
}
