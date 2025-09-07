// Dual IMX477 recorder: capture two CSI cameras with nvarguscamerasrc
// and record to two H.265 MKV files at up to 30fps at the highest
// resolution supported by the sensor.

#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

struct CamBranch {
  int sensor_id = 0;
  gchar *outfile = nullptr;
  GstElement *src = nullptr;
  GstElement *caps = nullptr;
  GstElement *queue = nullptr;
  GstElement *enc = nullptr;
  GstElement *parser = nullptr;
  GstElement *mux = nullptr;
  GstElement *sink = nullptr;
};

static GMainLoop *main_loop = nullptr;
static GstElement *g_pipeline = nullptr;

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

static void handle_sigint(int) {
  if (main_loop) {
    g_main_loop_quit(main_loop);
  }
}

static void print_usage(const char *prog) {
  g_print(
      "Usage: %s [--sensor0 N] [--sensor1 N] [--out0 file.mkv] [--out1 file.mkv]\\n"
      "            [--width W] [--height H] [--fps 30] [--bitrate kbps] [--sensor-mode M] [--duration-sec N]\\n"
      "            [--out-dir DIR] [--sync true|false]\\n"
      "Notes: Defaults assume IMX477 on Jetson (3840x2160@30).\\n"
      "       If unsure of sensor-mode, leave unset and use caps.\\n",
      prog);
}

static gboolean send_eos_cb(gpointer data) {
  GstElement *pipeline = GST_ELEMENT(data);
  g_print("Duration elapsed; sending EOS...\n");
  gst_element_send_event(pipeline, gst_event_new_eos());
  return G_SOURCE_REMOVE; // run once
}

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
  const gchar *out_dir = nullptr;
  gboolean filesink_sync = FALSE; // default: do not clock-sync file writing
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
    }
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { print_usage(argv[0]); return 0; }
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
    if (!out0_set) {
      gchar *name0 = g_strdup_printf("cam%d_%s.mkv", sensor0, ts);
      auto_out0 = g_build_filename(out_dir, name0, NULL);
      g_free(name0);
      out0 = auto_out0;
    }
    if (!out1_set) {
      gchar *name1 = g_strdup_printf("cam%d_%s.mkv", sensor1, ts);
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

    // nvarguscamerasrc
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

    // Queue for isolation
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
    cams[i].mux = gst_element_factory_make("matroskamux", name);
    if (!cams[i].mux) {
      g_printerr("Failed to create matroskamux for cam %d\n", i);
      return -1;
    }
    // Optional: make file playable before EOS
    g_object_set(cams[i].mux, "writing-app", "dual-record", "streamable", TRUE, NULL);

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
