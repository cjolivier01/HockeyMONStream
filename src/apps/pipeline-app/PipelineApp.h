#pragma once

/* clang-format off */
// X11 stuff must come first because it defines "Status"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#undef Status
/* clang-format on */

#include <cuda_runtime_api.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <functional>
#include <memory>
#include <vector>

#include "absl/status/status.h"

// Application and common headers.
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
typedef struct _NvDsFrameLatencyInfo NvDsFrameLatencyInfo;
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
  absl::Status createPipelines(std::vector<std::shared_ptr<HmApp>>& app_contexts, CleanupStack& cleanup_stack) const;
  absl::Status createMainLoop(std::vector<std::shared_ptr<HmApp>>& app_contexts, std::map<int, Window>& windows, CleanupStack& cleanup_stack);
  absl::Status playPipelines(std::vector<std::shared_ptr<HmApp>>& app_contexts, CleanupStack& cleanup_stack) const;
  absl::Status waitForPipelinesStopped(std::vector<std::shared_ptr<HmApp>>& app_contexts) const;

private:
  // Callback and helper functions.
  static void all_bbox_generated(AppCtx* app_ctx, GstBuffer* buf, NvDsBatchMeta* batch_meta, guint index);
  static void _intr_handler(int signum);
  void handle_intr(int signum);
  void _intr_setup();
  static void perf_cb_static(gpointer context, NvDsAppPerfStruct* str);
  void perf_cb(gpointer context, NvDsAppPerfStruct* str);
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

 private:
  // std::vector<std::unique_ptr<HmApp>> app_ctx_;
  std::map<long, std::vector<std::shared_ptr<HmApp>>> stage_app_contexts_;
  std::map<long, std::map</*instance_number=*/int, Window>> stage_windows_;

  long current_stage_{0};
  guint cintr_;
  GMainLoop* main_loop_;
  gchar** cfg_files_;
  gchar** input_uris_;
  gchar** game_id_;
  gboolean print_version_;
  gboolean show_bbox_text_;
  gboolean print_dependencies_version_;
  gboolean quit_;
  gboolean dump_pipeline_dot_;
  gboolean force_reconfigure_;
  gint return_value_;
  // guint num_instances_;
  guint num_input_uris_;
  GMutex fps_lock_;
  gdouble fps_[MAX_SOURCE_BINS];
  gdouble fps_avg_[MAX_SOURCE_BINS];
  Display* display_;
  GThread* x_event_thread_;
  GMutex disp_lock_;
  guint rrow_, rcol_, rcfg_;
  gboolean rrowsel_, selecting_;
  static constexpr const char* configure_stitching_config_file_name_ = "ds_hockey_configure_stitching.yaml";
  static PipelineApplication* instance_;
};
