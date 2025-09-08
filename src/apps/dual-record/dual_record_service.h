// Minimal service wrapper around the dual-camera recorder pipeline.
// Provides start/stop/status functions and runs the GLib main loop
// for the GStreamer pipeline in a background thread.

#pragma once

#include <gst/gst.h>
#include <glib.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

struct DualRecordOptions {
  int sensor0 = 0;
  int sensor1 = 1;
  int width = 3840;
  int height = 2160;
  int fps_n = 30;
  int fps_d = 1;
  int bitrate_kbps = 40000;
  int sensor_mode = -1; // -1: unset
  int duration_sec = 0; // 0: run until stop

  std::string out0 = "cam0.mkv";
  std::string out1 = "cam1.mkv";
  std::string container = "mkv"; // mkv|mp4
  std::string out_dir;            // optional, overrides names if set and out0/out1 not set explicitly
  bool filesink_sync = false;
  bool auto_30fps = false; // adjust 4032x3040@>21fps to 3840x2160@30

  // Per-camera overrides (0: leave auto)
  int exposure0_us = 0;
  int exposure1_us = 0;
  double gain0 = 0.0;
  double gain1 = 0.0;
};

class DualRecorderService {
 public:
  DualRecorderService();
  ~DualRecorderService();

  bool Start(const DualRecordOptions &opt, std::string *err);
  bool Stop(std::string *err);
  std::string Status() const;

 private:
  static gboolean BusCall(GstBus *bus, GstMessage *msg, gpointer data);
  static gboolean SendEosCb(gpointer data);

  bool BuildPipeline(const DualRecordOptions &opt, std::string *err);
  void LoopThread();

  // State
  std::atomic<bool> running_{false};
  GstElement *pipeline_ = nullptr;
  GMainLoop *loop_ = nullptr;
  std::thread loop_thread_;
};

