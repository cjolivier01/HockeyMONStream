#pragma once

/* clang-format off */
// X11 stuff must come first because it defines "Status"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "hstream/src/apps/apps-common/deepstream_sources.h"
#undef Status
/* clang-format on */

#include <cuda_runtime_api.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"

// Application and common headers.
#include "PlaybackProgress.h"
#include "TerminalProgressUi.h"
#include "configurator.h"
#include "deepstream_app.h"
#include "hstream/src/libs/common/pipeline_utils.h"

// Macro definitions.
#define APP_TITLE "DeepStream"
#define DEFAULT_X_WINDOW_WIDTH 1920
#define DEFAULT_X_WINDOW_HEIGHT 1080

// Forward declarations for DeepStream types.
typedef struct _AppCtx AppCtx;
typedef struct _NvDsBatchMeta NvDsBatchMeta;
typedef struct _NvDsFrameMeta NvDsFrameMeta;
typedef struct _NvDsObjectMeta NvDsObjectMeta;
typedef struct _NvDsAppPerfStruct NvDsAppPerfStruct;
// typedef struct _NvDsFrameLatencyInfo NvDsFrameLatencyInfo;
struct NvDsDisplayMeta;

//------------------------------------------------------------------------------
// CleanupStack: a simple RAII container for cleanup callbacks.
//------------------------------------------------------------------------------
class CleanupStack {
 public:
  /**
   * @brief Adds a cleanup callback to the stack.
   *
   * @param cleanup A function to call during cleanup.
   */
  void push(std::function<void()> cleanup);

  /**
   * @brief Destructor that executes cleanup callbacks in reverse order.
   */
  ~CleanupStack();

 private:
  std::vector<std::function<void()>> cleanups_;
};

//------------------------------------------------------------------------------
// PipelineApplication: encapsulates global state and pipeline functions.
//------------------------------------------------------------------------------
class PipelineApplication {
 public:
  PipelineApplication();
  ~PipelineApplication();

  /**
   * @brief Main run function to initialize and execute the application.
   *
   * @param argc Argument count.
   * @param argv Argument vector.
   * @return absl::Status indicating success or error.
   */
  absl::Status run(int argc, char* argv[]);

  // Helper functions for pipeline initialization and execution.
  absl::Status initializeInstances(CleanupStack& cleanup_stack);
  absl::Status configureInstances(size_t stage_index, std::vector<std::shared_ptr<HmApp>>& app_contexts);
  absl::Status createPipelines(std::vector<std::shared_ptr<HmApp>>& app_contexts, CleanupStack& cleanup_stack);
  absl::Status createMainLoop(
      std::vector<std::shared_ptr<HmApp>>& app_contexts,
      std::map<int, Window>& windows,
      CleanupStack& cleanup_stack);
  absl::Status playPipelines(std::vector<std::shared_ptr<HmApp>>& app_contexts, CleanupStack& cleanup_stack);
  absl::Status waitForPipelinesStopped(std::vector<std::shared_ptr<HmApp>>& app_contexts) const;
  absl::Status stopPipeline(std::shared_ptr<HmApp> app_context) const;

 private:
  // Callback and helper functions.
  static void all_bbox_generated(AppCtx* app_ctx, GstBuffer* buf, NvDsBatchMeta* batch_meta, guint index);
  static void _intr_handler(int signum);
  void handle_intr(int signum);
  void _intr_setup();
  static void perf_cb_static(gpointer context, NvDsAppPerfStruct* str);
  void perf_cb(gpointer context, NvDsAppPerfStruct* str);
  void record_timed_run_progress(uint64_t processed_ns);
  hm::PlaybackProgressMetrics collect_progress_metrics(AppCtx* app_ctx);
  std::string format_progress_status(const hm::PlaybackProgressMetrics& metrics) const;
  hm::TerminalProgressSnapshot make_terminal_progress_snapshot(
      AppCtx* app_ctx,
      NvDsAppPerfStruct* str,
      const hm::PlaybackProgressMetrics& metrics) const;
  hm::TerminalProgressGraphSnapshot build_progress_graph_snapshot(
      const std::vector<std::shared_ptr<HmApp>>& app_contexts) const;
  static gboolean check_for_interrupt_static(gpointer data);
  gboolean check_for_interrupt();
  static gboolean kbhit();
  static void changemode(int dir);
  void print_runtime_commands() const;
  bool read_stdin_char(char* out) const;
  bool read_runtime_command_line(std::string* line);
  bool handle_runtime_command_line(const std::string& line);
  void reset_playback_progress_rates(uint64_t generation);
  bool set_render_window_runtime(guint64 window_id);
  bool set_preview_active_runtime(const std::string& channel, guint64 generation);
  bool capture_preview_frame_runtime(const std::string& channel, const std::string& path);
  bool set_render_audio_muted_runtime(bool muted);
  bool set_element_property_runtime(
      const std::string& element_name,
      const std::string& property_name,
      const std::string& value);
  bool set_element_properties_runtime(
      const std::vector<std::tuple<std::string, std::string, std::string>>& assignments);
  static gboolean event_thread_func_static(gpointer arg);
  gboolean event_thread_func();
  static gboolean handle_element_message_static(AppCtx* app_ctx, GstMessage* message);
  gboolean handle_element_message(AppCtx* app_ctx, GstMessage* message);
  static gboolean rewind_after_stitching_calibration_static(gpointer arg);
  gboolean rewind_after_stitching_calibration(long stage, uint64_t main_loop_generation);
  void cancel_stitch_frame_rewind(uint64_t main_loop_generation);
  void reset_playback_timing_state(long stage);
  uint64_t initial_pipeline_position_ns(const HmApp* app_ctx) const;
  static int get_source_id_from_coordinates(float x_rel, float y_rel, AppCtx* app_ctx);
  static gpointer nvds_x_event_thread_static(gpointer data);
  gpointer nvds_x_event_thread();
  static gboolean overlay_graphics_static(AppCtx* app_ctx, GstBuffer* buf, NvDsBatchMeta* batch_meta, guint index);
  gboolean overlay_graphics(AppCtx* app_ctx, GstBuffer* buf, NvDsBatchMeta* batch_meta, guint index);
  static gboolean recreate_pipeline_thread_func_static(gpointer arg);
  gboolean recreate_pipeline_thread_func(gpointer arg);
  absl::Status auto_focus_cameras(const std::vector<std::shared_ptr<HmApp>>& app_contexts) const;
  absl::Status configure_source_preview_sinks(const std::vector<std::shared_ptr<HmApp>>& app_contexts);

 private:
  // std::vector<std::unique_ptr<HmApp>> app_ctx_;
  std::map<long, std::vector<std::shared_ptr<HmApp>>> stage_app_contexts_;
  std::map<long, std::map</*instance_number=*/int, Window>> stage_windows_;
  std::set<AppCtx*> one_pass_calibration_contexts_;

  long current_stage_{0};
  volatile sig_atomic_t cintr_;
  GMainLoop* main_loop_{nullptr};
  // Command-line options / configuration
  gchar** cfg_files_{nullptr};
  gchar** input_uris_{nullptr};
  gchar** game_id_{nullptr};
  gchar** enable_sources_{nullptr};
  gchar** enable_sinks_{nullptr};
  // TODO: how to work this into multiple configs? Maybe prefix with cfg file number?
  std::vector<std::map<std::string, std::string>> pipeline_options_;
  std::vector<std::set<NvDsSourceType>> enabled_source_types_;
  std::vector<std::set<NvDsSinkType>> enabled_sink_types_;
  gboolean print_version_;
  gboolean show_;
  gdouble show_stitching_scale_{-1};
  gdouble show_playtracker_scale_{-1};
  gdouble show_scaled_scale_{-1};
  gdouble show_render_scale_{-1};
  gint64 render_window_id_{0};
  gboolean headless_render_video_{FALSE};
  std::vector<guint64> source_render_window_ids_;
  std::map<std::string, guint64> ui_preview_window_ids_;
  std::string initial_ui_preview_channel_{"program"};
  struct UiPreviewChannel {
    GstElement* ingress_isolation{nullptr};
    GstElement* isolation{nullptr};
    GstElement* sink{nullptr};
  };
  std::map<std::string, UiPreviewChannel> ui_preview_channels_;
  std::string active_ui_preview_channel_;
  guint64 active_ui_preview_generation_{1};
  gdouble stitch_rotate_degrees_{0.0};
  gboolean stitch_rotate_degrees_set_{FALSE};
  gboolean show_bbox_text_;
  gboolean print_dependencies_version_;
  // Stop conditions
  gint time_limit_seconds_{0};
  gboolean quit_;
  gboolean dump_pipeline_dot_;
  gboolean force_reconfigure_;
  gboolean clean_stitching_artifacts_{FALSE};
  gboolean clean_stitching_from_control_points_{FALSE};
  gchar* clean_stitching_expected_invalidation_id_{nullptr};
  gboolean progress_ui_enabled_{FALSE};
  gboolean progress_ui_graph_{TRUE};
  gboolean progress_ui_no_graph_{FALSE};
  gboolean progress_ui_no_capture_{FALSE};
  gint progress_ui_lines_{11};
  gint progress_ui_refresh_ms_{1000};
  gint progress_ui_start_threshold_{0};
  gint return_value_;
  guint num_input_uris_;
  gint override_gpu_id_{hm::Configurator::kUseConfigFileGpu};
  GMutex fps_lock_;
  gdouble fps_[MAX_SOURCE_BINS];
  gdouble fps_avg_[MAX_SOURCE_BINS];
  struct ProgressState {
    bool initialized{false};
    uint64_t total_video_ns{GST_CLOCK_TIME_NONE};
    hm::PlaybackRateEstimator rate_estimator;
  };
  std::map<int, ProgressState> progress_states_;
  std::map<long, std::map<int, hm::PlaybackProgressMetrics>> ui_progress_by_stage_;
  uint64_t playback_progress_generation_{0};
  std::unique_ptr<hm::TerminalProgressUi> progress_ui_;
  std::mutex playback_timing_mu_;
  std::chrono::steady_clock::time_point timed_run_last_progress_wall_;
  uint64_t timed_run_last_progress_ns_{GST_CLOCK_TIME_NONE};
  // Display / event loop
  Display* display_ ABSL_GUARDED_BY(disp_lock_){nullptr};
  GThread* x_event_thread_;
  absl::Mutex disp_lock_;
  guint rrow_, rcol_, rcfg_;
  gboolean rrowsel_, selecting_;
  std::unique_ptr<std::thread> editor_thread_;
  uint64_t start_time_ns_{0};
  uint64_t stitch_frame_time_ns_{0};
  bool stitch_frame_time_set_{false};
  bool stitch_frame_time_loaded_from_config_{false};
  std::set<const AppCtx*> stitch_frame_rewound_contexts_;
  std::set<const AppCtx*> stitch_frame_rewind_pending_contexts_;
  std::atomic<bool> stitch_frame_calibration_active_{false};
  guint stitch_frame_rewind_source_id_{0};
  uint64_t main_loop_generation_{0};
  uint64_t first_pts_ns_{0};
  bool have_first_pts_{false};
  std::array<uint64_t, MAX_SOURCE_BINS> first_frame_numbers_by_source_{};
  std::array<bool, MAX_SOURCE_BINS> have_first_frame_by_source_{};
  bool runtime_command_active_{false};
  bool config_selection_active_{false};
  std::string runtime_command_buffer_;
  static constexpr const char* default_config_file_name_ = "configs/ds_hockey_app_config.yaml";
  static PipelineApplication* instance_;
};
