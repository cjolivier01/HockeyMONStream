#include <glib.h>
#include <gst/gst.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <iostream>

// Structure to hold the pipeline data
class AppData {
 public:
  GstElement* pipeline;
  GMainLoop* loop;
  bool is_deepstream;
  int src_width;
  int src_height;
  int dst_width;
  int dst_height;
  bool has_video;
  bool has_audio;
  bool video_linked;
  bool audio_linked;
  bool dot_file_generated;

  AppData()
      : pipeline(nullptr),
        loop(nullptr),
        is_deepstream(false),
        src_width(1920),
        src_height(1080),
        dst_width(960),
        dst_height(540),
        has_video(false),
        has_audio(false),
        video_linked(false),
        audio_linked(false),
        dot_file_generated(false) {}
};

// Forward declaration for state change callback
static gboolean state_change_callback(GstBus* bus, GstMessage* msg, gpointer data);

// Callback function for handling bus messages
static gboolean bus_call(GstBus* bus, GstMessage* msg, gpointer data) {
  AppData* app_data = static_cast<AppData*>(data);

  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
      std::cout << "End of stream" << std::endl;
      g_main_loop_quit(app_data->loop);
      break;
    case GST_MESSAGE_ERROR: {
      gchar* debug;
      GError* error;
      gst_message_parse_error(msg, &error, &debug);
      std::cerr << "ERROR from element " << GST_OBJECT_NAME(msg->src) << ": " << error->message << std::endl;
      if (debug) {
        std::cerr << "Error details: " << debug << std::endl;
        g_free(debug);
      }
      g_error_free(error);
      g_main_loop_quit(app_data->loop);
      break;
    }
    case GST_MESSAGE_WARNING: {
      gchar* debug;
      GError* error;
      gst_message_parse_warning(msg, &error, &debug);
      std::cerr << "WARNING from element " << GST_OBJECT_NAME(msg->src) << ": " << error->message << std::endl;
      if (debug) {
        std::cerr << "Warning details: " << debug << std::endl;
        g_free(debug);
      }
      g_error_free(error);
      break;
    }
    case GST_MESSAGE_STATE_CHANGED:
      // Handle state changes
      return state_change_callback(bus, msg, data);
    default:
      break;
  }
  return TRUE;
}

// Handler for state changes to generate the dot file
static gboolean state_change_callback(GstBus* bus, GstMessage* msg, gpointer data) {
  AppData* app_data = static_cast<AppData*>(data);
  GstState old_state, new_state, pending_state;

  // Only process state changes from the pipeline itself
  if (GST_MESSAGE_SRC(msg) == GST_OBJECT(app_data->pipeline)) {
    gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);

    // If we've entered the PAUSED state and haven't generated a dot file yet
    if (new_state == GST_STATE_PAUSED && !app_data->dot_file_generated) {
      std::cout << "Pipeline entered PAUSED state, generating DOT file..." << std::endl;

      // Generate the DOT file
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(app_data->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-paused");

      app_data->dot_file_generated = true;
      std::cout << "DOT file generated. To convert to an image: dot -Tpng -o pipeline.png pipeline-paused.dot"
                << std::endl;
      std::cout << "Note: Make sure GST_DEBUG_DUMP_DOT_DIR is set, e.g.: export GST_DEBUG_DUMP_DOT_DIR=." << std::endl;
    }
  }

  return TRUE;
}

// Function to check if DeepStream is available
static bool check_deepstream_available() {
  // Try to create DeepStream-specific elements
  GstElement* nvdec = gst_element_factory_make("nvv4l2decoder", nullptr);
  GstElement* nvconv = gst_element_factory_make("nvvideoconvert", nullptr);

  bool has_nvdec = (nvdec != nullptr);
  bool has_nvconv = (nvconv != nullptr);

  if (nvdec)
    gst_object_unref(nvdec);
  if (nvconv)
    gst_object_unref(nvconv);

  // Consider DeepStream available if at least the decoder and converter are present
  return (has_nvdec && has_nvconv);
}

// Helper function to print caps
static void print_caps(const GstCaps* caps, const char* prefix) {
  if (!caps) {
    std::cout << prefix << ": (NULL)" << std::endl;
    return;
  }

  gchar* caps_str = gst_caps_to_string(caps);
  std::cout << prefix << ": " << caps_str << std::endl;
  g_free(caps_str);
}

// Callback for demuxer pad added
static void on_pad_added(GstElement* element, GstPad* pad, gpointer data) {
  AppData* app_data = static_cast<AppData*>(data);
  GstCaps* caps;
  GstStructure* str;
  const gchar* name;
  GstPad* sink_pad = nullptr;
  GstElement* next_element = nullptr;

  // Get pad capabilities
  caps = gst_pad_get_current_caps(pad);
  if (!caps) {
    caps = gst_pad_query_caps(pad, nullptr);
  }

  // Get the structure from caps
  str = gst_caps_get_structure(caps, 0);
  name = gst_structure_get_name(str);

  std::cout << "Pad caps: " << gst_structure_to_string(str) << std::endl;

  // Check media type and link to appropriate pipeline branch
  if (g_str_has_prefix(name, "video/")) {
    if (app_data->video_linked) {
      std::cout << "Video already linked, skipping..." << std::endl;
      gst_caps_unref(caps);
      return;
    }

    // Only process HEVC video
    if (!g_str_has_prefix(name, "video/x-h265")) {
      std::cout << "Ignoring non-HEVC video stream: " << name << std::endl;
      gst_caps_unref(caps);
      return;
    }

    // Get video dimensions if available
    gst_structure_get_int(str, "width", &app_data->src_width);
    gst_structure_get_int(str, "height", &app_data->src_height);

    std::cout << "Detected input dimensions: " << app_data->src_width << "x" << app_data->src_height << std::endl;

    // Calculate output dimensions (half size)
    app_data->dst_width = app_data->src_width / 2;
    app_data->dst_height = app_data->src_height / 2;

    // Update capsfilter with the new dimensions
    GstElement* video_caps = gst_bin_get_by_name(GST_BIN(app_data->pipeline), "capsfilter");
    if (video_caps) {
      GstCaps* resize_caps = gst_caps_new_simple(
          "video/x-raw", "width", G_TYPE_INT, app_data->dst_width, "height", G_TYPE_INT, app_data->dst_height, NULL);

      std::cout << "Setting resize caps: ";
      print_caps(resize_caps, "Resize caps");

      g_object_set(G_OBJECT(video_caps), "caps", resize_caps, NULL);
      gst_caps_unref(resize_caps);
      gst_object_unref(video_caps);
    }

    // Link to h265parse element
    std::cout << "Linking HEVC video stream" << std::endl;

    GstElement* h265parse = gst_bin_get_by_name(GST_BIN(app_data->pipeline), "h265parse");

    if (h265parse) {
      // Get the sink pad of the h265parse element
      sink_pad = gst_element_get_static_pad(h265parse, "sink");

      // Check if sink pad is already linked
      GstPad* sink_pad_peer = gst_pad_get_peer(sink_pad);
      if (sink_pad_peer) {
        std::cout << "h265parse sink pad already has a peer, unlinking first" << std::endl;
        gst_pad_unlink(gst_pad_get_peer(sink_pad), sink_pad);
        gst_object_unref(sink_pad_peer);
      }

      // Try with caps conversion to convert from hvc1 to byte-stream format
      GstCaps* src_caps = gst_pad_get_current_caps(pad);
      GstCaps* converted_caps = gst_caps_copy(src_caps);

      // Modify caps to ensure compatibility with h265parse
      gst_caps_set_simple(
          converted_caps, "stream-format", G_TYPE_STRING, "byte-stream", "alignment", G_TYPE_STRING, "au", NULL);

      std::cout << "Modified caps for linking: ";
      print_caps(converted_caps, "Modified caps");

      // Try to directly link pads
      GstPadLinkReturn ret = gst_pad_link(pad, sink_pad);
      if (GST_PAD_LINK_FAILED(ret)) {
        std::cout << "Failed to link pads directly, return code: " << ret << std::endl;

        // Force caps for both pads
        gst_pad_set_caps(pad, converted_caps);
        gst_pad_set_caps(sink_pad, converted_caps);

        // Try again with modified caps
        ret = gst_pad_link(pad, sink_pad);
        if (GST_PAD_LINK_FAILED(ret)) {
          std::cout << "Failed to link pads with modified caps too, return code: " << ret << std::endl;
        } else {
          std::cout << "Successfully linked video pad with modified caps" << std::endl;
          app_data->video_linked = true;
          app_data->has_video = true;
        }
      } else {
        std::cout << "Successfully linked video pad directly" << std::endl;
        app_data->video_linked = true;
        app_data->has_video = true;
      }

      // Clean up
      gst_caps_unref(src_caps);
      gst_caps_unref(converted_caps);
      gst_object_unref(sink_pad);
      gst_object_unref(h265parse);
    } else {
      std::cout << "Could not find h265parse element" << std::endl;
    }
  } else if (g_str_has_prefix(name, "audio/")) {
    if (app_data->audio_linked) {
      std::cout << "Audio already linked, skipping..." << std::endl;
      gst_caps_unref(caps);
      return;
    }

    std::cout << "Linking audio stream" << std::endl;
    next_element = gst_bin_get_by_name(GST_BIN(app_data->pipeline), "aacparse");
    app_data->has_audio = true;

    // Get the sink pad of the next element
    if (next_element) {
      sink_pad = gst_element_get_static_pad(next_element, "sink");

      // Link the pads
      if (GST_PAD_LINK_FAILED(gst_pad_link(pad, sink_pad))) {
        std::cout << "Failed to link audio pads" << std::endl;
      } else {
        app_data->audio_linked = true;
        std::cout << "Successfully linked audio pad" << std::endl;
      }

      gst_object_unref(sink_pad);
      gst_object_unref(next_element);
    }
  } else {
    std::cout << "Ignoring unsupported pad: " << name << std::endl;
  }

  gst_caps_unref(caps);
}

int main(int argc, char* argv[]) {
  // Check arguments
  if (argc != 3) {
    std::cerr << "Usage: " << argv[0] << " <input-mp4-file> <output-mp4-file>" << std::endl;
    return -1;
  }

  // Print DOT file generation environment variable reminder
  std::cout << "To generate DOT files, make sure you've set: export GST_DEBUG_DUMP_DOT_DIR=." << std::endl;

  // Initialize GStreamer
  gst_init(&argc, &argv);

  // Create application data
  AppData app_data;

  // Check for DeepStream availability
  app_data.is_deepstream = check_deepstream_available();
  std::cout << "DeepStream hardware acceleration: " << (app_data.is_deepstream ? "Available" : "Not available")
            << std::endl;

  // Create the main loop
  app_data.loop = g_main_loop_new(NULL, FALSE);

  // Create pipeline
  app_data.pipeline = gst_pipeline_new("hevc-resize-pipeline");

  // Create elements
  GstElement* source = gst_element_factory_make("filesrc", "file-source");
  GstElement* demuxer = gst_element_factory_make("qtdemux", "demux");
  GstElement* h265parse = gst_element_factory_make("h265parse", "h265parse");
  GstElement* aacparse = gst_element_factory_make("aacparse", "aacparse");
  GstElement* video_decoder = nullptr;
  GstElement* video_converter = nullptr;
  GstElement* video_caps = gst_element_factory_make("capsfilter", "capsfilter");
  GstElement* video_encoder = nullptr;
  GstElement* h264parse = gst_element_factory_make("h264parse", "h264parse");
  GstElement* audio_decoder = gst_element_factory_make("avdec_aac", "audio_decoder");
  GstElement* audio_convert = gst_element_factory_make("audioconvert", "audio_convert");
  GstElement* audio_resample = gst_element_factory_make("audioresample", "audio_resample");
  GstElement* audio_encoder = gst_element_factory_make("avenc_aac", "audio_encoder");
  GstElement* muxer = gst_element_factory_make("qtmux", "mux");
  GstElement* sink = gst_element_factory_make("filesink", "file-sink");

  // Create hardware-specific elements
  if (app_data.is_deepstream) {
    video_decoder = gst_element_factory_make("nvv4l2decoder", "video_decoder");
    video_converter = gst_element_factory_make("nvvideoconvert", "video_converter");
    video_encoder = gst_element_factory_make("nvv4l2h264enc", "video_encoder");

    if (video_encoder) {
      // Configure hardware encoder
      g_object_set(G_OBJECT(video_encoder), "bitrate", 4000000, NULL); // 4 Mbps
    }
  } else {
    video_decoder = gst_element_factory_make("avdec_h265", "video_decoder");
    video_converter = gst_element_factory_make("videoconvert", "video_converter");
    video_encoder = gst_element_factory_make("x264enc", "video_encoder");

    if (video_encoder) {
      // Configure software encoder
      g_object_set(G_OBJECT(video_encoder), "bitrate", 2000, NULL); // 2 Mbps
      g_object_set(G_OBJECT(video_encoder), "tune", 0x4, NULL); // zerolatency
      g_object_set(G_OBJECT(video_encoder), "speed-preset", 1, NULL); // ultrafast
    }
  }

  // Check all elements were created
  if (!app_data.pipeline || !source || !demuxer || !h265parse || !aacparse || !video_decoder || !video_converter ||
      !video_caps || !video_encoder || !h264parse || !audio_decoder || !audio_convert || !audio_resample ||
      !audio_encoder || !muxer || !sink) {
    std::cerr << "One or more elements could not be created. Exiting." << std::endl;
    return -1;
  }

  // Set properties
  g_object_set(G_OBJECT(source), "location", argv[1], NULL);
  g_object_set(G_OBJECT(sink), "location", argv[2], NULL);

  // Set initial capsfilter
  GstCaps* init_caps = gst_caps_new_simple(
      "video/x-raw", "width", G_TYPE_INT, app_data.dst_width, "height", G_TYPE_INT, app_data.dst_height, NULL);
  g_object_set(G_OBJECT(video_caps), "caps", init_caps, NULL);
  print_caps(init_caps, "Initial capsfilter");
  gst_caps_unref(init_caps);

  // Add all elements to the pipeline
  gst_bin_add_many(
      GST_BIN(app_data.pipeline),
      source,
      demuxer,
      h265parse,
      video_decoder,
      video_converter,
      video_caps,
      video_encoder,
      h264parse,
      aacparse,
      audio_decoder,
      audio_convert,
      audio_resample,
      audio_encoder,
      muxer,
      sink,
      NULL);

  // Link static elements (those that can be linked before the pipeline starts)
  // Source to demuxer
  if (!gst_element_link(source, demuxer)) {
    std::cerr << "Failed to link source to demuxer" << std::endl;
    return -1;
  }

  // Link h265parse to video_decoder
  if (!gst_element_link(h265parse, video_decoder)) {
    std::cerr << "Failed to link h265parse to video decoder" << std::endl;
    return -1;
  }

  // Link video processing chain
  if (!gst_element_link_many(video_decoder, video_converter, video_caps, video_encoder, h264parse, NULL)) {
    std::cerr << "Failed to link video processing elements" << std::endl;
    return -1;
  }

  // Link h264parse to muxer
  if (!gst_element_link(h264parse, muxer)) {
    std::cerr << "Failed to link h264parse to muxer" << std::endl;
    return -1;
  }

  // Link audio processing chain
  if (!gst_element_link(aacparse, audio_decoder)) {
    std::cerr << "Failed to link aacparse to audio decoder" << std::endl;
    return -1;
  }

  if (!gst_element_link_many(audio_decoder, audio_convert, audio_resample, audio_encoder, NULL)) {
    std::cerr << "Failed to link audio processing elements" << std::endl;
    return -1;
  }

  // Link audio_encoder to muxer
  if (!gst_element_link(audio_encoder, muxer)) {
    std::cerr << "Failed to link audio_encoder to muxer" << std::endl;
    return -1;
  }

  // Link muxer to sink
  if (!gst_element_link(muxer, sink)) {
    std::cerr << "Failed to link muxer to sink" << std::endl;
    return -1;
  }

  // Connect the demuxer's pad-added signal for dynamic pad creation
  g_signal_connect(demuxer, "pad-added", G_CALLBACK(on_pad_added), &app_data);

  // Add bus watch
  GstBus* bus = gst_element_get_bus(app_data.pipeline);
  gst_bus_add_watch(bus, bus_call, &app_data);
  gst_object_unref(bus);

  // Start playing
  std::cout << "Starting pipeline..." << std::endl;
  gst_element_set_state(app_data.pipeline, GST_STATE_PLAYING);

  // Run the main loop
  std::cout << "Running..." << std::endl;
  g_main_loop_run(app_data.loop);

  // Clean up
  std::cout << "Cleaning up..." << std::endl;
  gst_element_set_state(app_data.pipeline, GST_STATE_NULL);
  gst_object_unref(GST_OBJECT(app_data.pipeline));
  g_main_loop_unref(app_data.loop);

  return 0;
}
