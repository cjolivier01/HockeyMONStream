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
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"

// Application and common headers.
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
  absl::Status createPipelines(std::vector<std::shared_ptr<HmApp>>& app_contexts, CleanupStack& cleanup_stack) const;
  absl::Status createMainLoop(
      std::vector<std::shared_ptr<HmApp>>& app_contexts,
      std::map<int, Window>& windows,
      CleanupStack& cleanup_stack);
  absl::Status playPipelines(std::vector<std::shared_ptr<HmApp>>& app_contexts, CleanupStack& cleanup_stack) const;
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
  std::string format_progress_status(AppCtx* app_ctx);
  static gboolean check_for_interrupt_static(gpointer data);
  gboolean check_for_interrupt();
  static gboolean kbhit();
  static void changemode(int dir);
  void print_runtime_commands() const;
  static gboolean event_thread_func_static(gpointer arg);
  gboolean event_thread_func();
  static int get_source_id_from_coordinates(float x_rel, float y_rel, AppCtx* app_ctx);
  static gpointer nvds_x_event_thread_static(gpointer data);
  gpointer nvds_x_event_thread();
  static gboolean overlay_graphics_static(AppCtx* app_ctx, GstBuffer* buf, NvDsBatchMeta* batch_meta, guint index);
  gboolean overlay_graphics(AppCtx* app_ctx, GstBuffer* buf, NvDsBatchMeta* batch_meta, guint index);
  static gboolean recreate_pipeline_thread_func_static(gpointer arg);
  gboolean recreate_pipeline_thread_func(gpointer arg);
  absl::Status auto_focus_cameras(const std::vector<std::shared_ptr<HmApp>>& app_contexts) const;

 private:
  // std::vector<std::unique_ptr<HmApp>> app_ctx_;
  std::map<long, std::vector<std::shared_ptr<HmApp>>> stage_app_contexts_;
  std::map<long, std::map</*instance_number=*/int, Window>> stage_windows_;

  long current_stage_{0};
  guint cintr_;
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
  gint return_value_;
  guint num_input_uris_;
  gint override_gpu_id_{hm::Configurator::kUseConfigFileGpu};
  GMutex fps_lock_;
  gdouble fps_[MAX_SOURCE_BINS];
  gdouble fps_avg_[MAX_SOURCE_BINS];
  struct ProgressState {
    bool initialized{false};
    bool have_speed_sample{false};
    uint64_t total_video_ns{GST_CLOCK_TIME_NONE};
    uint64_t speed_base_processed_ns{0};
    std::chrono::steady_clock::time_point speed_base_wall;
  };
  std::map<int, ProgressState> progress_states_;
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
  uint64_t first_pts_ns_{0};
  bool have_first_pts_{false};
  std::array<uint64_t, MAX_SOURCE_BINS> first_frame_numbers_by_source_{};
  std::array<bool, MAX_SOURCE_BINS> have_first_frame_by_source_{};
  static constexpr const char* default_config_file_name_ = "configs/ds_hockey_app_config.yaml";
  static PipelineApplication* instance_;
};
