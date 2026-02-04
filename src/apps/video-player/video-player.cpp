#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

struct PlayerPipeline {
  GstElement* pipeline = nullptr;
  GstElement* source = nullptr; // nvurisrcbin (DeepStream)

  GstElement* video_queue = nullptr;
  GstElement* video_convert = nullptr; // nvvideoconvert
  GstElement* video_caps = nullptr; // capsfilter (NVMM/NV12)
  GstElement* video_tee = nullptr; // tee

  GstElement* display_queue = nullptr;
  GstElement* egl_transform = nullptr; // optional nvegltransform (Jetson)
  GstElement* video_sink = nullptr; // nveglglessink / fakesink

  GstElement* encode_queue = nullptr;
  GstElement* encoder = nullptr; // nvv4l2h264enc
  GstElement* h264parse = nullptr;
  GstElement* mux = nullptr; // qtmux (optional)
  GstElement* encode_sink = nullptr; // filesink or fakesink

  GstElement* audio_queue = nullptr;
  GstElement* audio_convert = nullptr;
  GstElement* audio_resample = nullptr;
  GstElement* audio_volume = nullptr; // volume
  GstElement* audio_sink = nullptr;

  bool video_pad_linked = false;
  bool audio_pad_linked = false;

  guint bus_watch_id = 0;
};

struct AppUi {
  GtkWidget* window = nullptr;
  GtkWidget* video_area = nullptr;
  GtkWidget* play_pause_btn = nullptr;
  GtkWidget* stop_btn = nullptr;
  GtkWidget* open_btn = nullptr;
  GtkWidget* seek_scale = nullptr;
  GtkWidget* time_label = nullptr;
  GtkWidget* volume_scale = nullptr;
  GtkWidget* status_label = nullptr;
};

struct AppState {
  PlayerPipeline gst;
  AppUi ui;

  GMainLoop* main_loop = nullptr;
  guint ui_timer_id = 0;
  guint64 duration_ns = GST_CLOCK_TIME_NONE;
  bool user_dragging_seek = false;
  bool pipeline_error = false;

  guintptr x11_window_handle = 0;
  std::string current_uri;
  std::string record_out;

  bool headless = false;
  int time_limit_sec = 0;
};

static void prepend_env_path(const char* var, const std::string& path) {
  if (path.empty()) {
    return;
  }
  const char* cur = g_getenv(var);
  if (cur && std::strstr(cur, path.c_str())) {
    return;
  }
  std::string next = path;
  if (cur && *cur) {
    next.append(":").append(cur);
  }
  g_setenv(var, next.c_str(), TRUE);
}

static void ensure_deepstream_env() {
  const char* deepstream_root = "/opt/nvidia/deepstream/deepstream";
  if (!g_file_test(deepstream_root, G_FILE_TEST_IS_DIR)) {
    return;
  }
  prepend_env_path("GST_PLUGIN_PATH", std::string(deepstream_root) + "/lib/gst-plugins");
  prepend_env_path("LD_LIBRARY_PATH", std::string(deepstream_root) + "/lib");
}

static std::string to_uri(const std::string& input) {
  if (input.rfind("file://", 0) == 0 || input.rfind("rtsp://", 0) == 0 || input.rfind("http://", 0) == 0 ||
      input.rfind("https://", 0) == 0) {
    return input;
  }
  GError* error = nullptr;
  gchar* uri = g_filename_to_uri(input.c_str(), nullptr, &error);
  if (!uri) {
    std::string msg = error ? error->message : "unknown error";
    if (error) {
      g_error_free(error);
    }
    g_printerr("Failed to convert to URI: %s (%s)\n", input.c_str(), msg.c_str());
    return {};
  }
  std::string out(uri);
  g_free(uri);
  return out;
}

static std::string format_time_ns(gint64 ns) {
  if (ns < 0) {
    return "--:--";
  }
  const gint64 total_sec = ns / GST_SECOND;
  const gint64 hours = total_sec / 3600;
  const gint64 minutes = (total_sec % 3600) / 60;
  const gint64 seconds = total_sec % 60;
  char buf[32];
  if (hours > 0) {
    std::snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", (long)hours, (long)minutes, (long)seconds);
  } else {
    std::snprintf(buf, sizeof(buf), "%02ld:%02ld", (long)minutes, (long)seconds);
  }
  return buf;
}

static void set_status(AppState* app, const char* text) {
  if (!app || !app->ui.status_label) {
    return;
  }
  gtk_label_set_text(GTK_LABEL(app->ui.status_label), text ? text : "");
}

static void update_play_pause_label(AppState* app, bool playing) {
  if (!app || !app->ui.play_pause_btn) {
    return;
  }
  gtk_button_set_label(GTK_BUTTON(app->ui.play_pause_btn), playing ? "Pause" : "Play");
}

static void apply_video_overlay(AppState* app) {
  if (!app) {
    return;
  }
  GstElement* sink = app->gst.video_sink;
  if (!sink || app->x11_window_handle == 0) {
    return;
  }
  if (!GST_IS_VIDEO_OVERLAY(sink)) {
    g_printerr("Video sink does not implement GstVideoOverlay; embedding not available.\n");
    return;
  }
  gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), (gulong)app->x11_window_handle);
  gst_video_overlay_handle_events(GST_VIDEO_OVERLAY(sink), TRUE);
}

static void on_pad_added(GstElement* /*src*/, GstPad* new_pad, gpointer user_data) {
  AppState* app = static_cast<AppState*>(user_data);
  if (!app || !new_pad) {
    return;
  }

  GstCaps* caps = gst_pad_get_current_caps(new_pad);
  if (!caps) {
    caps = gst_pad_query_caps(new_pad, nullptr);
  }
  if (!caps || gst_caps_get_size(caps) == 0) {
    if (caps) {
      gst_caps_unref(caps);
    }
    return;
  }

  const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
  if (name && g_str_has_prefix(name, "video/") && !app->gst.video_pad_linked) {
    GstPad* sink_pad = gst_element_get_static_pad(app->gst.video_queue, "sink");
    if (!sink_pad) {
      gst_caps_unref(caps);
      return;
    }
    if (gst_pad_link(new_pad, sink_pad) != GST_PAD_LINK_OK) {
      g_printerr("Failed to link decoded video to video chain.\n");
    } else {
      app->gst.video_pad_linked = true;
    }
    gst_object_unref(sink_pad);
  } else if (name && g_str_has_prefix(name, "audio/") && !app->gst.audio_pad_linked && app->gst.audio_queue) {
    GstPad* sink_pad = gst_element_get_static_pad(app->gst.audio_queue, "sink");
    if (!sink_pad) {
      gst_caps_unref(caps);
      return;
    }
    if (gst_pad_link(new_pad, sink_pad) != GST_PAD_LINK_OK) {
      g_printerr("Failed to link decoded audio to audio chain.\n");
    } else {
      app->gst.audio_pad_linked = true;
    }
    gst_object_unref(sink_pad);
  }

  gst_caps_unref(caps);
}

static gboolean on_bus_message(GstBus* /*bus*/, GstMessage* msg, gpointer user_data) {
  AppState* app = static_cast<AppState*>(user_data);
  if (!app || !msg) {
    return TRUE;
  }

  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError* err = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(msg, &err, &debug);
      g_printerr("GStreamer error: %s\n", err ? err->message : "unknown");
      if (debug) {
        g_printerr("Debug: %s\n", debug);
      }
      if (app->ui.status_label) {
        set_status(app, err ? err->message : "GStreamer error");
      }
      if (err) {
        g_error_free(err);
      }
      if (debug) {
        g_free(debug);
      }
      app->pipeline_error = true;
      if (app->main_loop) {
        g_main_loop_quit(app->main_loop);
      }
      break;
    }
    case GST_MESSAGE_EOS:
      if (app->main_loop) {
        g_main_loop_quit(app->main_loop);
      } else {
        set_status(app, "End of stream");
        update_play_pause_label(app, false);
        if (app->gst.pipeline) {
          gst_element_set_state(app->gst.pipeline, GST_STATE_PAUSED);
          gst_element_seek_simple(
              app->gst.pipeline,
              GST_FORMAT_TIME,
              (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
              0);
        }
      }
      break;
    case GST_MESSAGE_DURATION_CHANGED:
      app->duration_ns = GST_CLOCK_TIME_NONE;
      break;
    case GST_MESSAGE_STATE_CHANGED: {
      if (GST_MESSAGE_SRC(msg) == GST_OBJECT(app->gst.pipeline)) {
        GstState old_state, new_state, pending_state;
        gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
        (void)old_state;
        (void)pending_state;
        if (new_state == GST_STATE_PLAYING) {
          update_play_pause_label(app, true);
          set_status(app, "Playing");
        } else if (new_state == GST_STATE_PAUSED) {
          update_play_pause_label(app, false);
          set_status(app, "Paused");
        }
      }
      break;
    }
    default:
      break;
  }
  return TRUE;
}

static gboolean ui_update_cb(gpointer user_data) {
  AppState* app = static_cast<AppState*>(user_data);
  if (!app || !app->gst.pipeline) {
    return G_SOURCE_CONTINUE;
  }

  if (!app->ui.seek_scale || !app->ui.time_label) {
    return G_SOURCE_CONTINUE;
  }

  if (app->duration_ns == GST_CLOCK_TIME_NONE) {
    gint64 dur = 0;
    if (gst_element_query_duration(app->gst.pipeline, GST_FORMAT_TIME, &dur)) {
      app->duration_ns = (guint64)dur;
      const double dur_sec = (double)dur / (double)GST_SECOND;
      gtk_range_set_range(GTK_RANGE(app->ui.seek_scale), 0.0, std::max(0.0, dur_sec));
    }
  }

  gint64 pos = 0;
  if (!gst_element_query_position(app->gst.pipeline, GST_FORMAT_TIME, &pos)) {
    return G_SOURCE_CONTINUE;
  }

  if (!app->user_dragging_seek) {
    const double pos_sec = (double)pos / (double)GST_SECOND;
    gtk_range_set_value(GTK_RANGE(app->ui.seek_scale), std::max(0.0, pos_sec));
  }

  const std::string pos_s = format_time_ns(pos);
  const std::string dur_s =
      (app->duration_ns == GST_CLOCK_TIME_NONE) ? std::string("--:--") : format_time_ns((gint64)app->duration_ns);
  std::string label = pos_s + " / " + dur_s;
  gtk_label_set_text(GTK_LABEL(app->ui.time_label), label.c_str());

  return G_SOURCE_CONTINUE;
}

static gboolean headless_timeout_cb(gpointer user_data) {
  AppState* app = static_cast<AppState*>(user_data);
  if (!app || !app->gst.pipeline) {
    return G_SOURCE_REMOVE;
  }
  g_print("Time limit reached; sending EOS.\n");
  gst_element_send_event(app->gst.pipeline, gst_event_new_eos());
  return G_SOURCE_REMOVE;
}

static void destroy_pipeline(AppState* app) {
  if (!app) {
    return;
  }

  if (app->gst.bus_watch_id != 0 && app->gst.pipeline) {
    g_source_remove(app->gst.bus_watch_id);
    app->gst.bus_watch_id = 0;
  }

  if (app->gst.pipeline) {
    gst_element_set_state(app->gst.pipeline, GST_STATE_NULL);
    gst_object_unref(app->gst.pipeline);
  }

  app->gst = PlayerPipeline{};
  app->duration_ns = GST_CLOCK_TIME_NONE;
}

static bool link_tee_to_queue(GstElement* tee, GstElement* queue) {
  GstPad* tee_src = gst_element_request_pad_simple(tee, "src_%u");
  if (!tee_src) {
    return false;
  }
  GstPad* queue_sink = gst_element_get_static_pad(queue, "sink");
  if (!queue_sink) {
    gst_object_unref(tee_src);
    return false;
  }
  const GstPadLinkReturn ret = gst_pad_link(tee_src, queue_sink);
  gst_object_unref(queue_sink);
  gst_object_unref(tee_src);
  return ret == GST_PAD_LINK_OK;
}

static bool build_pipeline(AppState* app, const std::string& uri, bool with_ui_video_sink) {
  if (!app) {
    return false;
  }

  destroy_pipeline(app);
  app->pipeline_error = false;

  app->gst.pipeline = gst_pipeline_new("hstream_video_player");
  app->gst.source = gst_element_factory_make("nvurisrcbin", "source");
  app->gst.video_queue = gst_element_factory_make("queue", "video_queue");
  app->gst.video_convert = gst_element_factory_make("nvvideoconvert", "video_convert");
  app->gst.video_caps = gst_element_factory_make("capsfilter", "video_caps");
  app->gst.video_tee = gst_element_factory_make("tee", "video_tee");

  app->gst.display_queue = gst_element_factory_make("queue", "display_queue");
  if (with_ui_video_sink) {
    app->gst.egl_transform = gst_element_factory_make("nvegltransform", "egl_transform");
    app->gst.video_sink = gst_element_factory_make("nveglglessink", "video_sink");
  } else {
    app->gst.video_sink = gst_element_factory_make("fakesink", "video_sink");
  }

  app->gst.encode_queue = gst_element_factory_make("queue", "encode_queue");
  app->gst.encoder = gst_element_factory_make("nvv4l2h264enc", "encoder");
  app->gst.h264parse = gst_element_factory_make("h264parse", "h264parse");

  app->gst.audio_queue = gst_element_factory_make("queue", "audio_queue");
  app->gst.audio_convert = gst_element_factory_make("audioconvert", "audio_convert");
  app->gst.audio_resample = gst_element_factory_make("audioresample", "audio_resample");
  app->gst.audio_volume = gst_element_factory_make("volume", "audio_volume");
  app->gst.audio_sink = gst_element_factory_make("autoaudiosink", "audio_sink");

  if (!app->gst.pipeline || !app->gst.source || !app->gst.video_queue || !app->gst.video_convert || !app->gst.video_caps ||
      !app->gst.video_tee || !app->gst.display_queue || !app->gst.video_sink || !app->gst.encode_queue || !app->gst.encoder ||
      !app->gst.h264parse) {
    g_printerr("Failed to create one or more required GStreamer elements.\n");
    return false;
  }

  g_object_set(G_OBJECT(app->gst.source), "uri", uri.c_str(), nullptr);
  g_object_set(G_OBJECT(app->gst.source), "disable-audio", FALSE, nullptr);
  g_signal_connect(app->gst.source, "pad-added", G_CALLBACK(on_pad_added), app);

  g_object_set(G_OBJECT(app->gst.encoder), "insert-sps-pps", 1, nullptr);
  g_object_set(G_OBJECT(app->gst.encoder), "iframeinterval", 30, nullptr);
  g_object_set(G_OBJECT(app->gst.encoder), "bitrate", 8000, nullptr); // kbps

  if (!with_ui_video_sink) {
    g_object_set(G_OBJECT(app->gst.video_sink), "sync", FALSE, nullptr);
  } else {
    g_object_set(G_OBJECT(app->gst.video_sink), "sync", FALSE, nullptr);
    g_object_set(G_OBJECT(app->gst.video_sink), "qos", FALSE, nullptr);
  }

  GstCaps* caps = gst_caps_from_string("video/x-raw(memory:NVMM), format=NV12");
  if (!caps) {
    g_printerr("Failed to create NVMM caps.\n");
    return false;
  }
  g_object_set(G_OBJECT(app->gst.video_caps), "caps", caps, nullptr);
  gst_caps_unref(caps);

  if (!app->record_out.empty()) {
    app->gst.mux = gst_element_factory_make("qtmux", "mux");
    app->gst.encode_sink = gst_element_factory_make("filesink", "encode_sink");
    if (!app->gst.mux || !app->gst.encode_sink) {
      g_printerr("Failed to create recording elements (qtmux/filesink).\n");
      return false;
    }
    g_object_set(G_OBJECT(app->gst.encode_sink), "location", app->record_out.c_str(), nullptr);
    g_object_set(G_OBJECT(app->gst.encode_sink), "sync", FALSE, nullptr);
  } else {
    app->gst.encode_sink = gst_element_factory_make("fakesink", "encode_sink");
    if (!app->gst.encode_sink) {
      g_printerr("Failed to create encode fakesink.\n");
      return false;
    }
    g_object_set(G_OBJECT(app->gst.encode_sink), "sync", FALSE, nullptr);
  }

  gst_bin_add_many(
      GST_BIN(app->gst.pipeline),
      app->gst.source,
      app->gst.video_queue,
      app->gst.video_convert,
      app->gst.video_caps,
      app->gst.video_tee,
      app->gst.display_queue,
      app->gst.encode_queue,
      app->gst.encoder,
      app->gst.h264parse,
      app->gst.encode_sink,
      app->gst.audio_queue,
      app->gst.audio_convert,
      app->gst.audio_resample,
      app->gst.audio_volume,
      app->gst.audio_sink,
      nullptr);

  if (with_ui_video_sink) {
    if (app->gst.egl_transform) {
      gst_bin_add_many(GST_BIN(app->gst.pipeline), app->gst.egl_transform, app->gst.video_sink, nullptr);
    } else {
      gst_bin_add(GST_BIN(app->gst.pipeline), app->gst.video_sink);
    }
  } else {
    gst_bin_add(GST_BIN(app->gst.pipeline), app->gst.video_sink);
  }

  if (app->gst.mux) {
    gst_bin_add(GST_BIN(app->gst.pipeline), app->gst.mux);
  }

  if (!gst_element_link_many(app->gst.video_queue, app->gst.video_convert, app->gst.video_caps, app->gst.video_tee, nullptr)) {
    g_printerr("Failed to link video decode chain (queue->nvvideoconvert->capsfilter->tee).\n");
    return false;
  }

  if (!link_tee_to_queue(app->gst.video_tee, app->gst.display_queue)) {
    g_printerr("Failed to link tee to display queue.\n");
    return false;
  }

  if (!link_tee_to_queue(app->gst.video_tee, app->gst.encode_queue)) {
    g_printerr("Failed to link tee to encode queue.\n");
    return false;
  }

  if (with_ui_video_sink && app->gst.egl_transform) {
    if (!gst_element_link_many(app->gst.display_queue, app->gst.egl_transform, app->gst.video_sink, nullptr)) {
      g_printerr("Failed to link display branch.\n");
      return false;
    }
  } else {
    if (!gst_element_link_many(app->gst.display_queue, app->gst.video_sink, nullptr)) {
      g_printerr("Failed to link display branch.\n");
      return false;
    }
  }

  if (app->gst.mux) {
    if (!gst_element_link_many(app->gst.encode_queue, app->gst.encoder, app->gst.h264parse, app->gst.mux, app->gst.encode_sink, nullptr)) {
      g_printerr("Failed to link encode branch (mp4).\n");
      return false;
    }
  } else {
    if (!gst_element_link_many(app->gst.encode_queue, app->gst.encoder, app->gst.h264parse, app->gst.encode_sink, nullptr)) {
      g_printerr("Failed to link encode branch (discard).\n");
      return false;
    }
  }

  if (!gst_element_link_many(
          app->gst.audio_queue,
          app->gst.audio_convert,
          app->gst.audio_resample,
          app->gst.audio_volume,
          app->gst.audio_sink,
          nullptr)) {
    g_printerr("Failed to link audio branch.\n");
    // Audio is optional; keep going.
  }

  GstBus* bus = gst_element_get_bus(app->gst.pipeline);
  app->gst.bus_watch_id = gst_bus_add_watch(bus, on_bus_message, app);
  gst_object_unref(bus);

  if (with_ui_video_sink) {
    apply_video_overlay(app);
  }

  if (gst_element_set_state(app->gst.pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Failed to set pipeline to PAUSED.\n");
    return false;
  }
  if (gst_element_set_state(app->gst.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Failed to set pipeline to PLAYING.\n");
    return false;
  }

  set_status(app, app->record_out.empty() ? "Playing (encode: discard)" : "Playing (recording)");
  return true;
}

static void on_video_area_realize(GtkWidget* widget, gpointer user_data) {
  AppState* app = static_cast<AppState*>(user_data);
  if (!app || !widget) {
    return;
  }
  GdkWindow* window = gtk_widget_get_window(widget);
  if (!window) {
    return;
  }
  app->x11_window_handle = (guintptr)GDK_WINDOW_XID(window);
  apply_video_overlay(app);
}

static void on_video_area_size_allocate(GtkWidget* widget, GdkRectangle* alloc, gpointer user_data) {
  AppState* app = static_cast<AppState*>(user_data);
  if (!app || !alloc) {
    return;
  }
  if (!app->gst.video_sink || app->x11_window_handle == 0 || !GST_IS_VIDEO_OVERLAY(app->gst.video_sink)) {
    return;
  }
  (void)widget;
  gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(app->gst.video_sink), 0, 0, alloc->width, alloc->height);
}

static void on_window_destroy(GtkWidget* /*widget*/, gpointer user_data) {
  AppState* app = static_cast<AppState*>(user_data);
  if (!app) {
    gtk_main_quit();
    return;
  }
  if (app->ui_timer_id != 0) {
    g_source_remove(app->ui_timer_id);
    app->ui_timer_id = 0;
  }
  destroy_pipeline(app);
  gtk_main_quit();
}

static void do_seek(AppState* app, double seconds) {
  if (!app || !app->gst.pipeline) {
    return;
  }
  const gint64 target = (gint64)std::llround(seconds * (double)GST_SECOND);
  gst_element_seek_simple(
      app->gst.pipeline,
      GST_FORMAT_TIME,
      (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
      std::max<gint64>(0, target));
}

static gboolean on_seek_button_press(GtkWidget* /*widget*/, GdkEventButton* /*event*/, gpointer user_data) {
  AppState* app = static_cast<AppState*>(user_data);
  if (app) {
    app->user_dragging_seek = true;
  }
  return FALSE;
}

static gboolean on_seek_button_release(GtkWidget* widget, GdkEventButton* /*event*/, gpointer user_data) {
  AppState* app = static_cast<AppState*>(user_data);
  if (!app || !widget) {
    return FALSE;
  }
  const double seconds = gtk_range_get_value(GTK_RANGE(widget));
  app->user_dragging_seek = false;
  do_seek(app, seconds);
  return FALSE;
}

static void on_volume_changed(GtkRange* range, gpointer user_data) {
  AppState* app = static_cast<AppState*>(user_data);
  if (!app || !app->gst.audio_volume) {
    return;
  }
  const double v = gtk_range_get_value(range) / 100.0;
  g_object_set(G_OBJECT(app->gst.audio_volume), "volume", v, nullptr);
}

static void on_play_pause_clicked(GtkButton* /*button*/, gpointer user_data) {
  AppState* app = static_cast<AppState*>(user_data);
  if (!app || !app->gst.pipeline) {
    return;
  }

  GstState state = GST_STATE_NULL;
  gst_element_get_state(app->gst.pipeline, &state, nullptr, 0);
  if (state == GST_STATE_PLAYING) {
    gst_element_set_state(app->gst.pipeline, GST_STATE_PAUSED);
  } else {
    gst_element_set_state(app->gst.pipeline, GST_STATE_PLAYING);
  }
}

static void on_stop_clicked(GtkButton* /*button*/, gpointer user_data) {
  AppState* app = static_cast<AppState*>(user_data);
  if (!app || !app->gst.pipeline) {
    return;
  }
  gst_element_set_state(app->gst.pipeline, GST_STATE_PAUSED);
  do_seek(app, 0.0);
  update_play_pause_label(app, false);
  set_status(app, "Stopped");
}

static void on_open_clicked(GtkButton* /*button*/, gpointer user_data) {
  AppState* app = static_cast<AppState*>(user_data);
  if (!app || !app->ui.window) {
    return;
  }

  GtkWidget* dialog = gtk_file_chooser_dialog_new(
      "Open Media",
      GTK_WINDOW(app->ui.window),
      GTK_FILE_CHOOSER_ACTION_OPEN,
      "_Cancel",
      GTK_RESPONSE_CANCEL,
      "_Open",
      GTK_RESPONSE_ACCEPT,
      nullptr);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_ACCEPT) {
    gtk_widget_destroy(dialog);
    return;
  }

  char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
  gtk_widget_destroy(dialog);
  if (!filename) {
    return;
  }
  std::string uri = to_uri(filename);
  g_free(filename);
  if (uri.empty()) {
    set_status(app, "Failed to open file");
    return;
  }

  app->current_uri = uri;
  const bool ok = build_pipeline(app, uri, /*with_ui_video_sink=*/true);
  if (!ok) {
    set_status(app, "Failed to start pipeline");
  }
}

static void build_ui(AppState* app) {
  app->ui.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(app->ui.window), "HStream Video Player");
  gtk_window_set_default_size(GTK_WINDOW(app->ui.window), 1280, 720);
  g_signal_connect(app->ui.window, "destroy", G_CALLBACK(on_window_destroy), app);

  GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(app->ui.window), root);

  GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_container_set_border_width(GTK_CONTAINER(toolbar), 6);
  gtk_box_pack_start(GTK_BOX(root), toolbar, FALSE, FALSE, 0);

  app->ui.open_btn = gtk_button_new_with_label("Open");
  g_signal_connect(app->ui.open_btn, "clicked", G_CALLBACK(on_open_clicked), app);
  gtk_box_pack_start(GTK_BOX(toolbar), app->ui.open_btn, FALSE, FALSE, 0);

  app->ui.play_pause_btn = gtk_button_new_with_label("Play");
  g_signal_connect(app->ui.play_pause_btn, "clicked", G_CALLBACK(on_play_pause_clicked), app);
  gtk_box_pack_start(GTK_BOX(toolbar), app->ui.play_pause_btn, FALSE, FALSE, 0);

  app->ui.stop_btn = gtk_button_new_with_label("Stop");
  g_signal_connect(app->ui.stop_btn, "clicked", G_CALLBACK(on_stop_clicked), app);
  gtk_box_pack_start(GTK_BOX(toolbar), app->ui.stop_btn, FALSE, FALSE, 0);

  GtkWidget* volume_label = gtk_label_new("Vol");
  gtk_box_pack_start(GTK_BOX(toolbar), volume_label, FALSE, FALSE, 0);

  app->ui.volume_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 1.0);
  gtk_widget_set_size_request(app->ui.volume_scale, 160, -1);
  gtk_range_set_value(GTK_RANGE(app->ui.volume_scale), 100.0);
  gtk_scale_set_draw_value(GTK_SCALE(app->ui.volume_scale), FALSE);
  g_signal_connect(app->ui.volume_scale, "value-changed", G_CALLBACK(on_volume_changed), app);
  gtk_box_pack_end(GTK_BOX(toolbar), app->ui.volume_scale, FALSE, FALSE, 0);

  app->ui.video_area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(app->ui.video_area, TRUE);
  gtk_widget_set_vexpand(app->ui.video_area, TRUE);
  gtk_box_pack_start(GTK_BOX(root), app->ui.video_area, TRUE, TRUE, 0);
  g_signal_connect(app->ui.video_area, "realize", G_CALLBACK(on_video_area_realize), app);
  g_signal_connect(app->ui.video_area, "size-allocate", G_CALLBACK(on_video_area_size_allocate), app);

  GtkWidget* controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_container_set_border_width(GTK_CONTAINER(controls), 6);
  gtk_box_pack_start(GTK_BOX(root), controls, FALSE, FALSE, 0);

  app->ui.seek_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.1);
  gtk_scale_set_draw_value(GTK_SCALE(app->ui.seek_scale), FALSE);
  gtk_widget_set_hexpand(app->ui.seek_scale, TRUE);
  gtk_box_pack_start(GTK_BOX(controls), app->ui.seek_scale, TRUE, TRUE, 0);
  gtk_widget_add_events(app->ui.seek_scale, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
  g_signal_connect(app->ui.seek_scale, "button-press-event", G_CALLBACK(on_seek_button_press), app);
  g_signal_connect(app->ui.seek_scale, "button-release-event", G_CALLBACK(on_seek_button_release), app);

  app->ui.time_label = gtk_label_new("--:-- / --:--");
  gtk_box_pack_start(GTK_BOX(controls), app->ui.time_label, FALSE, FALSE, 0);

  app->ui.status_label = gtk_label_new("Open a file to start");
  gtk_box_pack_start(GTK_BOX(root), app->ui.status_label, FALSE, FALSE, 0);

  gtk_widget_show_all(app->ui.window);
}

static void print_usage(const char* prog) {
  g_print(
      "Usage: %s [--input <path|uri>] [--record-out <file.mp4>] [--headless] [--time-limit <sec>]\\n"
      "\\n"
      "Notes:\\n"
      "  - Source decode uses DeepStream nvurisrcbin (hardware-accelerated).\\n"
      "  - Video encode always uses NVIDIA hardware encoder (nvv4l2h264enc).\\n"
      "  - Without --record-out, encoded H.264 is discarded (fakesink).\\n",
      prog);
}

} // namespace

int main(int argc, char** argv) {
  ensure_deepstream_env();
  gst_init(&argc, &argv);

  AppState app;

  std::string input;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    }
    if (arg == "--headless") {
      app.headless = true;
      continue;
    }
    if (arg == "--input" && i + 1 < argc) {
      input = argv[++i];
      continue;
    }
    if (arg == "--record-out" && i + 1 < argc) {
      app.record_out = argv[++i];
      continue;
    }
    if (arg == "--time-limit" && i + 1 < argc) {
      app.time_limit_sec = std::atoi(argv[++i]);
      continue;
    }
    if (!arg.empty() && arg[0] != '-' && input.empty()) {
      input = arg;
      continue;
    }
  }

  if (!gst_element_factory_find("nvurisrcbin")) {
    g_printerr("DeepStream nvurisrcbin is not available. Check your DeepStream/GStreamer installation.\n");
    return 2;
  }
  if (!gst_element_factory_find("nvv4l2h264enc")) {
    g_printerr("No NVIDIA hardware H.264 encoder found (nvv4l2h264enc).\n");
    return 2;
  }

  if (app.headless) {
    if (input.empty()) {
      g_printerr("--headless requires --input <path|uri>\n");
      return 2;
    }
    const std::string uri = to_uri(input);
    if (uri.empty()) {
      return 2;
    }
    if (!build_pipeline(&app, uri, /*with_ui_video_sink=*/false)) {
      return 1;
    }

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    app.main_loop = loop;
    if (app.time_limit_sec > 0) {
      g_timeout_add_seconds(app.time_limit_sec, headless_timeout_cb, &app);
    }
    g_main_loop_run(loop);
    app.main_loop = nullptr;
    g_main_loop_unref(loop);
    destroy_pipeline(&app);
    return app.pipeline_error ? 1 : 0;
  }

  if (!gtk_init_check(&argc, &argv)) {
    g_printerr("GTK init failed (no display?). Try --headless.\n");
    return 2;
  }

  build_ui(&app);
  app.ui_timer_id = g_timeout_add(200, ui_update_cb, &app);

  if (!input.empty()) {
    app.current_uri = to_uri(input);
    if (!app.current_uri.empty()) {
      if (!build_pipeline(&app, app.current_uri, /*with_ui_video_sink=*/true)) {
        set_status(&app, "Failed to start pipeline");
      }
    }
  }

  gtk_main();
  return 0;
}
