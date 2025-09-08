#include "dual_record_service.h"

#include <sys/stat.h>

#include <sstream>

namespace {
static void set_capsfilter(GstElement *caps_elem, int width, int height, int fps_n, int fps_d) {
  gchar *caps_str = g_strdup_printf(
      "video/x-raw(memory:NVMM), format=NV12, width=%d, height=%d, framerate=%d/%d",
      width, height, fps_n, fps_d);
  GstCaps *caps = gst_caps_from_string(caps_str);
  g_free(caps_str);
  g_object_set(caps_elem, "caps", caps, NULL);
  gst_caps_unref(caps);
}

static void ensure_dir(const std::string &dir) {
  if (dir.empty()) return;
  g_mkdir_with_parents(dir.c_str(), 0755);
}

static std::string timestamped_name(int sensor_id, const std::string &dir, const std::string &ext) {
  GDateTime *now = g_date_time_new_now_local();
  gchar *ts = g_date_time_format(now, "%Y%m%d_%H%M%S");
  g_date_time_unref(now);
  gchar *name = g_strdup_printf("cam%d_%s%s", sensor_id, ts, ext.c_str());
  gchar *full = g_build_filename(dir.c_str(), name, NULL);
  std::string out(full);
  g_free(ts);
  g_free(name);
  g_free(full);
  return out;
}
} // namespace

DualRecorderService::DualRecorderService() {}

DualRecorderService::~DualRecorderService() { std::string err; Stop(&err); }

gboolean DualRecorderService::BusCall(GstBus *, GstMessage *msg, gpointer data) {
  DualRecorderService *self = static_cast<DualRecorderService *>(data);
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
      g_main_loop_quit(self->loop_);
      break;
    case GST_MESSAGE_ERROR: {
      gchar *debug = nullptr;
      GError *err = nullptr;
      gst_message_parse_error(msg, &err, &debug);
      g_printerr("dual-recordd error: %s\n", err ? err->message : "");
      if (debug) { g_printerr("debug: %s\n", debug); g_free(debug); }
      if (err) g_error_free(err);
      g_main_loop_quit(self->loop_);
      break;
    }
    default:
      break;
  }
  return TRUE;
}

gboolean DualRecorderService::SendEosCb(gpointer data) {
  GstElement *pipeline = GST_ELEMENT(data);
  gst_element_send_event(pipeline, gst_event_new_eos());
  return G_SOURCE_REMOVE;
}

bool DualRecorderService::BuildPipeline(const DualRecordOptions &opt, std::string *err) {
  if (pipeline_) return true;

  int width = opt.width;
  int height = opt.height;
  if (opt.auto_30fps) {
    if ((width >= 4000 && height >= 3000 && opt.fps_n >= 30) || (width == 0 && height == 0)) {
      width = 3840; height = 2160; // IMX477 30fps
    }
  }

  pipeline_ = gst_pipeline_new("dual-recorderd");
  if (!pipeline_) { if (err) *err = "Failed to create pipeline"; return false; }

  ensure_dir(opt.out_dir);
  const std::string ext = (!g_ascii_strcasecmp(opt.container.c_str(), "mp4")) ? ".mp4" : ".mkv";
  std::string out0 = opt.out0;
  std::string out1 = opt.out1;
  if (!opt.out_dir.empty()) {
    if (out0 == "cam0.mkv") out0 = timestamped_name(opt.sensor0, opt.out_dir, ext);
    if (out1 == "cam1.mkv") out1 = timestamped_name(opt.sensor1, opt.out_dir, ext);
  }

  struct Branch { GstElement *src,*caps,*queue,*enc,*parser,*mux,*sink; } b[2]{};
  int sensors[2] = {opt.sensor0, opt.sensor1};
  const std::string outs[2] = {out0, out1};
  const int exp_us[2] = {opt.exposure0_us, opt.exposure1_us};
  const double gains[2] = {opt.gain0, opt.gain1};

  for (int i = 0; i < 2; ++i) {
    gchar name[64];
    snprintf(name, sizeof(name), "src%d", i);
    b[i].src = gst_element_factory_make("nvarguscamerasrc", name);
    if (!b[i].src) { if (err) *err = "nvarguscamerasrc unavailable"; return false; }
    g_object_set(b[i].src, "sensor-id", sensors[i], NULL);
    if (opt.sensor_mode >= 0) g_object_set(b[i].src, "sensor-mode", opt.sensor_mode, NULL);
    if (exp_us[i] > 0) {
      gchar *v = g_strdup_printf("%d %d", exp_us[i], exp_us[i]);
      g_object_set(b[i].src, "exposuretimerange", v, NULL);
      g_free(v);
    }
    if (gains[i] > 0.0) {
      gchar *v = g_strdup_printf("%.2f %.2f", gains[i], gains[i]);
      g_object_set(b[i].src, "gainrange", v, NULL);
      g_free(v);
    }

    snprintf(name, sizeof(name), "caps%d", i);
    b[i].caps = gst_element_factory_make("capsfilter", name);
    if (!b[i].caps) { if (err) *err = "capsfilter create failed"; return false; }
    set_capsfilter(b[i].caps, width, height, opt.fps_n, opt.fps_d);

    snprintf(name, sizeof(name), "queue%d", i);
    b[i].queue = gst_element_factory_make("queue", name);
    if (!b[i].queue) { if (err) *err = "queue create failed"; return false; }
    g_object_set(b[i].queue, "max-size-buffers", 0, "max-size-bytes", 0, "max-size-time", 0, NULL);

    snprintf(name, sizeof(name), "enc%d", i);
    b[i].enc = gst_element_factory_make("nvv4l2h265enc", name);
    if (!b[i].enc) { if (err) *err = "encoder create failed"; return false; }
    g_object_set(b[i].enc,
                 "bitrate", opt.bitrate_kbps * 1000,
                 "control-rate", 1,
                 "iframeinterval", opt.fps_n,
                 "preset-level", 1,
                 "insert-sps-pps", TRUE,
                 "maxperf-enable", TRUE,
                 NULL);

    snprintf(name, sizeof(name), "h265parse%d", i);
    b[i].parser = gst_element_factory_make("h265parse", name);
    if (!b[i].parser) { if (err) *err = "h265parse create failed"; return false; }

    snprintf(name, sizeof(name), "mux%d", i);
    if (!g_ascii_strcasecmp(opt.container.c_str(), "mp4")) {
      b[i].mux = gst_element_factory_make("qtmux", name);
    } else {
      b[i].mux = gst_element_factory_make("matroskamux", name);
    }
    if (!b[i].mux) { if (err) *err = "mux create failed"; return false; }
    if (!g_ascii_strcasecmp(opt.container.c_str(), "mp4")) {
      g_object_set(b[i].mux, "faststart", TRUE, NULL);
    } else {
      g_object_set(b[i].mux, "writing-app", "dual-recordd", "streamable", TRUE, NULL);
    }

    snprintf(name, sizeof(name), "sink%d", i);
    b[i].sink = gst_element_factory_make("filesink", name);
    if (!b[i].sink) { if (err) *err = "filesink create failed"; return false; }
    g_object_set(b[i].sink, "location", outs[i].c_str(), "sync", opt.filesink_sync, "async", FALSE, NULL);

    gst_bin_add_many(GST_BIN(pipeline_), b[i].src, b[i].caps, b[i].queue, b[i].enc, b[i].parser, b[i].mux, b[i].sink, NULL);
    if (!gst_element_link_many(b[i].src, b[i].caps, b[i].queue, b[i].enc, b[i].parser, b[i].mux, b[i].sink, NULL)) {
      if (err) *err = "link failed"; return false; }
  }

  gst_element_set_start_time(pipeline_, GST_CLOCK_TIME_NONE);

  loop_ = g_main_loop_new(NULL, FALSE);
  GstBus *bus = gst_element_get_bus(pipeline_);
  gst_bus_add_watch(bus, DualRecorderService::BusCall, this);
  gst_object_unref(bus);

  if (opt.duration_sec > 0) {
    g_timeout_add_seconds((guint)opt.duration_sec, DualRecorderService::SendEosCb, pipeline_);
  }
  return true;
}

void DualRecorderService::LoopThread() { g_main_loop_run(loop_); }

bool DualRecorderService::Start(const DualRecordOptions &opt, std::string *err) {
  if (running_) { if (err) *err = "already running"; return false; }
  if (!pipeline_) {
    if (!BuildPipeline(opt, err)) { return false; }
  }
  GstStateChangeReturn sret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  if (sret == GST_STATE_CHANGE_FAILURE) { if (err) *err = "PLAYING failed"; return false; }
  running_ = true;
  loop_thread_ = std::thread(&DualRecorderService::LoopThread, this);
  return true;
}

bool DualRecorderService::Stop(std::string * /*err*/) {
  if (!pipeline_) return true;
  gst_element_send_event(pipeline_, gst_event_new_eos());
  if (loop_) {
    if (loop_thread_.joinable()) loop_thread_.join();
    g_main_loop_unref(loop_);
    loop_ = nullptr;
  }
  gst_element_set_state(pipeline_, GST_STATE_NULL);
  gst_object_unref(GST_OBJECT(pipeline_));
  pipeline_ = nullptr;
  running_ = false;
  return true;
}

std::string DualRecorderService::Status() const {
  if (running_) return "RUNNING";
  if (pipeline_) return "READY";
  return "IDLE";
}

