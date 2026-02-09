/**
 * @file dual_record_service.h
 * @brief Minimal service wrapper around the dual-camera recorder pipeline.
 *
 * The DualRecorderService builds a two-branch GStreamer pipeline using
 * `nvarguscamerasrc` on Jetson platforms and provides simple control methods:
 * Start/Stop/Status. The GLib main loop for the pipeline runs in a background
 * thread so callers can interact with the service synchronously.
 */

#pragma once

#include <gst/gst.h>
#include <glib.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

/**
 * @brief Configuration options for the dual-camera recording service.
 */
struct DualRecordOptions {
  /** Index of the first CSI sensor (usually 0). */
  int sensor0 = 0;
  /** Index of the second CSI sensor (usually 1). */
  int sensor1 = 1;
  /** Requested capture width in pixels (caps constraint). */
  int width = 3840;
  /** Requested capture height in pixels (caps constraint). */
  int height = 2160;
  /** Framerate numerator (e.g., 30 for 30/1). */
  int fps_n = 30;
  /** Framerate denominator (e.g., 1 for 30/1). */
  int fps_d = 1;
  /** Encoder target bitrate in kbps per stream. */
  int bitrate_kbps = 40000;
  /** Optional Argus sensor mode; -1 to leave unset and rely on caps. */
  int sensor_mode = -1; // -1: unset
  /** Optional auto-stop after N seconds; 0 to run until Stop(). */
  int duration_sec = 0; // 0: run until stop

  /** Output file for camera 0 (overridden if out_dir is set and default name). */
  std::string out0 = "cam0.mkv";
  /** Output file for camera 1 (overridden if out_dir is set and default name). */
  std::string out1 = "cam1.mkv";
  /** Container format: "mkv" or "mp4". */
  std::string container = "mkv"; // mkv|mp4
  /** Optional directory; if set, filenames are timestamped unless explicitly given. */
  std::string out_dir;            // optional, overrides names if set and out0/out1 not set explicitly
  /** Whether the filesink should be clock-synchronized. */
  bool filesink_sync = false;
  /** If true, coerce 4K sensors to 3840x2160@30 when a >=30 fps mode is requested. */
  bool auto_30fps = false; // adjust 4032x3040@>21fps to 3840x2160@30

  // Per-camera overrides (0: leave auto)
  /** Exposure override for camera 0 in microseconds; 0 keeps auto-exposure. */
  int exposure0_us = 0;
  /** Exposure override for camera 1 in microseconds; 0 keeps auto-exposure. */
  int exposure1_us = 0;
  /** Analog gain override for camera 0; 0 keeps auto-gain. */
  double gain0 = 0.0;
  /** Analog gain override for camera 1; 0 keeps auto-gain. */
  double gain1 = 0.0;
};

/**
 * @brief Service that manages a dual-camera GStreamer recording pipeline.
 *
 * Typical usage:
 * - Construct DualRecorderService
 * - Call Start() with desired DualRecordOptions
 * - Optionally query Status()
 * - Call Stop() to tear down the pipeline
 */
class DualRecorderService {
 public:
  /** Construct an idle service (no pipeline built yet). */
  DualRecorderService();
  /** Stops and tears down any running pipeline on destruction. */
  ~DualRecorderService();

  /**
   * @brief Start the pipeline with the provided options.
   * @param opt Recording and camera options.
   * @param err Optional error string; set on failure.
   * @return true if the pipeline started successfully, false otherwise.
   */
  bool Start(const DualRecordOptions &opt, std::string *err);
  /**
   * @brief Stop the pipeline and join the main loop thread.
   * @param err Optional error string (unused currently).
   * @return true always (idempotent).
   */
  bool Stop(std::string *err);
  /**
   * @brief Get a human-readable status string.
   * @return "RUNNING", "READY", or "IDLE".
   */
  std::string Status() const;

 private:
  /** GStreamer bus callback; stops the main loop on EOS or ERROR. */
  static gboolean BusCall(GstBus *bus, GstMessage *msg, gpointer data);
  /** GLib timeout callback to send EOS after a fixed duration. */
  static gboolean SendEosCb(gpointer data);

  /**
   * @brief Build the dual-branch pipeline according to options.
   * @param opt Options used to configure sources, encoders, and sinks.
   * @param err Optional error string; set on failure.
   * @return true on success; false if any element creation or link fails.
   */
  bool BuildPipeline(const DualRecordOptions &opt, std::string *err);
  /** Run the GLib main loop (blocking). Invoked on a background thread. */
  void LoopThread();

  // State
  /** True after Start() until Stop() completes. */
  std::atomic<bool> running_{false};
  /** Root pipeline element (owned). */
  GstElement *pipeline_ = nullptr;
  /** GLib main loop created per pipeline run. */
  GMainLoop *loop_ = nullptr;
  /** Thread that runs LoopThread(). */
  std::thread loop_thread_;
};
