/**
 * @file dual_record.cpp
 * @brief Standalone dual-camera recorder using GStreamer on Jetson.
 *
 * Captures from two CSI sensors via `nvarguscamerasrc` and records to two
 * files (H.265 in MKV/MP4). Each camera branch is independent within the same
 * pipeline to share a common clock. Suitable for simple two-stream capture.
 */

#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

/**
 * @brief Per-camera elements and settings for a single pipeline branch.
 */
struct CamBranch {
  /** Argus sensor index for this branch. */
  int sensor_id = 0;
  /** Output filename (owned). */
  gchar *outfile = nullptr;
  /** Source: nvarguscamerasrc. */
  GstElement *src = nullptr;
  /** Caps filter to request format/geometry/framerate. */
  GstElement *caps = nullptr;
  /** Queue to decouple downstream IO latency. */
  GstElement *queue = nullptr;
  /** Hardware encoder (nvv4l2h265enc). */
  GstElement *enc = nullptr;
  /** Parser (h265parse). */
  GstElement *parser = nullptr;
  /** Container muxer (matroskamux or qtmux). */
  GstElement *mux = nullptr;
  /** File sink. */
  GstElement *sink = nullptr;
};

static GMainLoop *main_loop = nullptr;
static GstElement *g_pipeline = nullptr;

/**
 * @brief GStreamer bus watch; exits the main loop on EOS/ERROR.
 */
static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
  (void)bus;
  GMainLoop *loop = (GMainLoop *)data;

  switch (GST_MESSAGE_TYPE(msg)) {
  case GST_MESSAGE_EOS:
    g_print("End of stream\n");
    g_main_loop_quit(loop);
    break;
  case GST_MESSAGE_ERROR: {
    gchar *debug;
    GError *err;

    gst_message_parse_error(msg, &err, &debug);
    g_printerr("Error: %s\n", err->message);
    if (debug) {
      g_printerr("Debug details: %s\n", debug);
      g_free(debug);
    }
    g_error_free(err);
    g_main_loop_quit(loop);
    break;
  }
  default:
    break;
  }

  return TRUE;
}

/** SIGINT/SIGTERM handler to quit the main loop cleanly. */
static void handle_sigint(int) {
  if (main_loop) {
    g_main_loop_quit(main_loop);
  }
}

/** Print CLI usage. */
static void print_usage(const char *prog) {
  g_print(
      "Usage: %s [--sensor0 N] [--sensor1 N] [--out0 file.mkv] [--out1 file.mkv]\\n"
      "            [--width W] [--height H] [--fps 30] [--bitrate kbps] [--sensor-mode M] [--duration-sec N]\\n"
      "            [--out-dir DIR] [--sync true|false] [--container mkv|mp4] [--exposure-us N] [--gain F]\\n"
      "Notes: Defaults assume IMX477 on Jetson (3840x2160@30).\\n"
      "       If unsure of sensor-mode, leave unset and use caps.\\n",
      prog);
}

/** Timeout callback: inject EOS after --duration-sec elapses. */
static gboolean send_eos_cb(gpointer data) {
  GstElement *pipeline = GST_ELEMENT(data);
  g_print("Duration elapsed; sending EOS...\n");
  gst_element_send_event(pipeline, gst_event_new_eos());
  return G_SOURCE_REMOVE; // run once
}

/**
 * @brief CLI entry point for dual camera recording.
 *
 * Parses command-line options, builds a 2-branch pipeline, and records until
 * EOS or a signal is received. See print_usage() for available flags.
 */
int main(int argc, char *argv[]) {
  // Defaults for IMX477: highest 30fps mode (commonly 3840x2160@30)
  gint sensor0 = 0;
  gint sensor1 = 1;
  gint width = 3840;
  gint height = 2160;
  gint fps_n = 30;
  gint fps_d = 1;
  gint bitrate_kbps = 40000; // 40 Mbps per stream (tweak as needed)
  gint sensor_mode = -1;     // -1: don't set (use caps)
  gint duration_sec = 0;     // 0 means run until Ctrl+C or EOS
  gboolean auto_30fps = FALSE; // prefer 3840x2160@30 for IMX477 when >=30fps requested
  const gchar *out_dir = nullptr;
  gboolean filesink_sync = FALSE; // default: do not clock-sync file writing
  const gchar *container = "mkv"; // mkv|mp4
  gint exposure_us = 0;           // 0: leave auto-exposure (global)
  gdouble analog_gain = 0.0;      // 0: leave auto-gain (global)
  gint exposure0_us = 0;
  gint exposure1_us = 0;
  gdouble gain0 = 0.0;
  gdouble gain1 = 0.0;
  const gchar *out0 = "cam0.mkv";
  const gchar *out1 = "cam1.mkv";
  gboolean out0_set = FALSE;
  gboolean out1_set = FALSE;

  // Basic arg parsing
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--sensor0") && i + 1 < argc) sensor0 = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--sensor1") && i + 1 < argc) sensor1 = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--out0") && i + 1 < argc) { out0 = argv[++i]; out0_set = TRUE; }
    else if (!strcmp(argv[i], "--out1") && i + 1 < argc) { out1 = argv[++i]; out1_set = TRUE; }
    else if (!strcmp(argv[i], "--width") && i + 1 < argc) width = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--height") && i + 1 < argc) height = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
      fps_n = atoi(argv[++i]);
      fps_d = 1;
    } else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) bitrate_kbps = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--sensor-mode") && i + 1 < argc) sensor_mode = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--duration-sec") && i + 1 < argc) duration_sec = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--out-dir") && i + 1 < argc) out_dir = argv[++i];
    else if ((!strcmp(argv[i], "--sync") || !strcmp(argv[i], "--filesink-sync")) && i + 1 < argc) {
      const char *v = argv[++i];
      filesink_sync = (!g_ascii_strcasecmp(v, "true") || !strcmp(v, "1") || !g_ascii_strcasecmp(v, "yes"));
    } else if (!strcmp(argv[i], "--container") && i + 1 < argc) {
      container = argv[++i];
    } else if (!strcmp(argv[i], "--exposure-us") && i + 1 < argc) {
      exposure_us = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--gain") && i + 1 < argc) {
      analog_gain = g_ascii_strtod(argv[++i], NULL);
    } else if (!strcmp(argv[i], "--auto-30fps")) {
      auto_30fps = TRUE;
    } else if (!strcmp(argv[i], "--exposure0-us") && i + 1 < argc) {
      exposure0_us = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--exposure1-us") && i + 1 < argc) {
      exposure1_us = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--gain0") && i + 1 < argc) {
      gain0 = g_ascii_strtod(argv[++i], NULL);
    } else if (!strcmp(argv[i], "--gain1") && i + 1 < argc) {
      gain1 = g_ascii_strtod(argv[++i], NULL);
    }
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { print_usage(argv[0]); return 0; }
  }

  if (auto_30fps) {
    if ((width >= 4000 && height >= 3000 && fps_n >= 30) || (width == 0 && height == 0)) {
      width = 3840; height = 2160;
    }
  }

  // If out-dir provided, build timestamped filenames for unspecified outputs
  gchar *auto_out0 = nullptr;
  gchar *auto_out1 = nullptr;
  if (out_dir) {
    // Ensure directory exists
    g_mkdir_with_parents(out_dir, 0755);
    GDateTime *now = g_date_time_new_now_local();
    gchar *ts = g_date_time_format(now, "%Y%m%d_%H%M%S");
    g_date_time_unref(now);
    const char *ext = (!g_ascii_strcasecmp(container, "mp4")) ? ".mp4" : ".mkv";
    if (!out0_set) {
      gchar *name0 = g_strdup_printf("cam%d_%s%s", sensor0, ts, ext);
      auto_out0 = g_build_filename(out_dir, name0, NULL);
      g_free(name0);
      out0 = auto_out0;
    }
    if (!out1_set) {
      gchar *name1 = g_strdup_printf("cam%d_%s%s", sensor1, ts, ext);
      auto_out1 = g_build_filename(out_dir, name1, NULL);
      g_free(name1);
      out1 = auto_out1;
    }
    g_free(ts);
  }

  gst_init(&argc, &argv);

  signal(SIGINT, handle_sigint);
  signal(SIGTERM, handle_sigint);

  main_loop = g_main_loop_new(NULL, FALSE);

  GstElement *pipeline = gst_pipeline_new("dual-recorder");
  if (!pipeline) {
    g_printerr("Failed to create pipeline\n");
    return -1;
  }
  g_pipeline = pipeline;

  // Build two independent branches in a single pipeline to share the same clock
  CamBranch cams[2];
  cams[0].sensor_id = sensor0;
  cams[1].sensor_id = sensor1;
  cams[0].outfile = g_strdup(out0);
  cams[1].outfile = g_strdup(out1);

  for (int i = 0; i < 2; ++i) {
    gchar name[64];

    // Source: nvarguscamerasrc
    snprintf(name, sizeof(name), "src%d", i);
    cams[i].src = gst_element_factory_make("nvarguscamerasrc", name);
    if (!cams[i].src) {
      g_printerr("Failed to create nvarguscamerasrc for cam %d\n", i);
      return -1;
    }
    g_object_set(cams[i].src, "sensor-id", cams[i].sensor_id, NULL);
    if (sensor_mode >= 0) {
      g_object_set(cams[i].src, "sensor-mode", sensor_mode, NULL);
    }
    int cam_exp = exposure_us;
    if (i == 0 && exposure0_us > 0) cam_exp = exposure0_us;
    if (i == 1 && exposure1_us > 0) cam_exp = exposure1_us;
    if (cam_exp > 0) {
      gchar *v = g_strdup_printf("%d %d", cam_exp, cam_exp);
      g_object_set(cams[i].src, "exposuretimerange", v, NULL);
      g_free(v);
    }
    double cam_gain = analog_gain;
    if (i == 0 && gain0 > 0.0) cam_gain = gain0;
    if (i == 1 && gain1 > 0.0) cam_gain = gain1;
    if (cam_gain > 0.0) {
      gchar *v = g_strdup_printf("%.2f %.2f", cam_gain, cam_gain);
      g_object_set(cams[i].src, "gainrange", v, NULL);
      g_free(v);
    }

    // Caps filter to request NVMM NV12 at desired resolution and fps
    snprintf(name, sizeof(name), "caps%d", i);
    cams[i].caps = gst_element_factory_make("capsfilter", name);
    if (!cams[i].caps) {
      g_printerr("Failed to create capsfilter for cam %d\n", i);
      return -1;
    }
    gchar *caps_str = g_strdup_printf(
        "video/x-raw(memory:NVMM), format=NV12, width=%d, height=%d, framerate=%d/%d",
        width, height, fps_n, fps_d);
    GstCaps *caps = gst_caps_from_string(caps_str);
    g_free(caps_str);
    g_object_set(cams[i].caps, "caps", caps, NULL);
    gst_caps_unref(caps);

    // Queue for isolation between source/encode and file IO
    snprintf(name, sizeof(name), "queue%d", i);
    cams[i].queue = gst_element_factory_make("queue", name);
    if (!cams[i].queue) {
      g_printerr("Failed to create queue for cam %d\n", i);
      return -1;
    }
    g_object_set(cams[i].queue, "max-size-buffers", 0, "max-size-bytes", 0, "max-size-time", 0, NULL);

    // Hardware H.265 encoder
    snprintf(name, sizeof(name), "enc%d", i);
    cams[i].enc = gst_element_factory_make("nvv4l2h265enc", name);
    if (!cams[i].enc) {
      g_printerr("Failed to create nvv4l2h265enc for cam %d\n", i);
      return -1;
    }
    // Configure for quality at 30fps; tune as needed
    g_object_set(cams[i].enc,
                 "bitrate", bitrate_kbps * 1000, // in bps
                 "control-rate", 1,              // 0=Disable, 1=CBR, 2=VBR
                 "iframeinterval", fps_n,        // 1s GOP
                 "preset-level", 1,              // 1=Default, 2=HighQuality
                 "insert-sps-pps", TRUE,
                 "maxperf-enable", TRUE,
                 NULL);

    // Parser and muxer
    snprintf(name, sizeof(name), "h265parse%d", i);
    cams[i].parser = gst_element_factory_make("h265parse", name);
    if (!cams[i].parser) {
      g_printerr("Failed to create h265parse for cam %d\n", i);
      return -1;
    }

    snprintf(name, sizeof(name), "mux%d", i);
    if (!g_ascii_strcasecmp(container, "mp4")) {
      cams[i].mux = gst_element_factory_make("qtmux", name);
    } else {
      cams[i].mux = gst_element_factory_make("matroskamux", name);
    }
    if (!cams[i].mux) {
      g_printerr("Failed to create mux for cam %d\n", i);
      return -1;
    }
    // Optional container knobs
    if (!g_ascii_strcasecmp(container, "mp4")) {
      g_object_set(cams[i].mux, "faststart", TRUE, NULL);
    } else {
      g_object_set(cams[i].mux, "writing-app", "dual-record", "streamable", TRUE, NULL);
    }

    // File sink
    snprintf(name, sizeof(name), "sink%d", i);
    cams[i].sink = gst_element_factory_make("filesink", name);
    if (!cams[i].sink) {
      g_printerr("Failed to create filesink for cam %d\n", i);
      return -1;
    }
    g_object_set(cams[i].sink, "location", cams[i].outfile, "sync", filesink_sync, "async", FALSE, NULL);

    // Add to pipeline
    gst_bin_add_many(GST_BIN(pipeline), cams[i].src, cams[i].caps, cams[i].queue,
                     cams[i].enc, cams[i].parser, cams[i].mux, cams[i].sink, NULL);

    // Link elements in branch: src -> caps -> queue -> enc -> parse -> mux -> sink
    if (!gst_element_link_many(cams[i].src, cams[i].caps, cams[i].queue, cams[i].enc,
                               cams[i].parser, cams[i].mux, cams[i].sink, NULL)) {
      g_printerr("Failed linking elements for cam %d\n", i);
      return -1;
    }
  }

  // Share a single pipeline clock implicitly; ensure start-time is NONE so both start together
  gst_element_set_start_time(pipeline, GST_CLOCK_TIME_NONE);

  // Bus watch
  GstBus *bus = gst_element_get_bus(pipeline);
  gst_bus_add_watch(bus, bus_call, main_loop);
  gst_object_unref(bus);

  g_print("Starting recording: cam0 -> %s, cam1 -> %s\n", out0, out1);
  g_print("Requested %dx%d @ %d/%d fps, bitrate %d kbps per stream\n",
          width, height, fps_n, fps_d, bitrate_kbps);

  GstStateChangeReturn sret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
  if (sret == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Failed to set pipeline to PLAYING\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(pipeline));
    return -1;
  }

  if (duration_sec > 0) {
    g_print("Auto-stop after %d seconds enabled\n", duration_sec);
    g_timeout_add_seconds((guint)duration_sec, send_eos_cb, pipeline);
  }

  g_main_loop_run(main_loop);

  // Teardown
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(GST_OBJECT(pipeline));

  g_free(cams[0].outfile);
  g_free(cams[1].outfile);
  g_free(auto_out0);
  g_free(auto_out1);
  g_main_loop_unref(main_loop);
  main_loop = nullptr;

  return 0;
}
