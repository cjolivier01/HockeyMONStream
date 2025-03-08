/* clang-format off */
// X11 stuff must come first because it defines "Status"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#undef Status
/* clang-format on */

#include "hstream/src/apps/apps-common/deepstream_app_version.h"
#include "hstream/src/apps/apps-common/deepstream_common.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/pipeline_utils.h"

#include <cuda_runtime_api.h>
#include <gst/gstbin.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <functional> // For std::function
#include <memory>
#include <vector>
#include "deepstream_app.h"
#include "nvds_version.h"

#include <X11/keysym.h>
#include <gst/video/videooverlay.h>
#include <signal.h>
#include <sys/select.h>

#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"

// Macro definitions.
#define APP_TITLE "DeepStream"
#define DEFAULT_X_WINDOW_WIDTH 1920
#define DEFAULT_X_WINDOW_HEIGHT 1080

// Do not encapsulate this debug category per instructions.
GST_DEBUG_CATEGORY(NVDS_APP);

/**
 * @brief A simple RAII container for cleanup callbacks.
 *
 * When a CleanupStack is destroyed, all callbacks stored in it are invoked in reverse order.
 */
class CleanupStack {
 public:
  /**
   * @brief Adds a cleanup callback to the stack.
   *
   * @param cleanup A function to call during cleanup.
   */
  void push(std::function<void()> cleanup) {
    cleanups_.push_back(std::move(cleanup));
  }

  /**
   * @brief Destructor that executes cleanup callbacks in reverse order.
   */
  ~CleanupStack() {
    // Iterate in reverse order to call cleanups.
    for (auto it = cleanups_.rbegin(); it != cleanups_.rend(); ++it) {
      (*it)();
    }
  }

 private:
  /// Container for cleanup callback functions.
  std::vector<std::function<void()>> cleanups_;
};

/**
 * @brief Encapsulates all global state and callback functions for the pipeline application.
 *
 * This class manages the creation, execution, and cleanup of pipelines.
 */
class PipelineApplication {
 public:
  /**
   * @brief Constructor that initializes member variables.
   */
  PipelineApplication() {
    cintr_ = FALSE;
    main_loop_ = nullptr;
    cfg_files_ = nullptr;
    input_uris_ = nullptr;
    game_id_ = nullptr;
    print_version_ = FALSE;
    show_bbox_text_ = FALSE;
    print_dependencies_version_ = FALSE;
    quit_ = FALSE;
    dump_pipeline_dot_ = FALSE;
    force_reconfigure_ = FALSE;
    return_value_ = 0;
    num_instances_ = 0;
    num_input_uris_ = 0;
    memset(fps_, 0, sizeof(fps_));
    memset(fps_avg_, 0, sizeof(fps_avg_));
    display_ = nullptr;
    // Vectors are default–initialized.
    x_event_thread_ = nullptr;
    rrow_ = rcol_ = rcfg_ = 0;
    rrowsel_ = FALSE;
    selecting_ = FALSE;
    g_mutex_init(&fps_lock_);
    g_mutex_init(&disp_lock_);
    instance_ = this;
  }

  /**
   * @brief Destructor to clear resources.
   */
  ~PipelineApplication() {
    g_mutex_clear(&disp_lock_);
  }

  /**
   * @brief Main run function to initialize and execute the application.
   *
   * @param argc Argument count.
   * @param argv Argument vector.
   * @return absl::Status indicating success or error.
   */
  absl::Status run(int argc, char* argv[]);

  //--------------------------------------------------------------------------
  // Helper functions for pipeline initialization and execution.
  //--------------------------------------------------------------------------

  /**
   * @brief Initializes application instances.
   *
   * Section 1: Create and initialize each HmApp instance.
   *
   * @param cleanup_stack Reference to a CleanupStack for registering cleanup tasks.
   * @return absl::Status indicating success or error.
   */
  absl::Status initializeInstances(CleanupStack& cleanup_stack); // Section 1

  /**
   * @brief Creates pipelines for each instance.
   *
   * Section 2: Create pipelines for each instance.
   *
   * @param cleanup_stack Reference to a CleanupStack for registering cleanup tasks.
   * @return absl::Status indicating success or error.
   */
  absl::Status createPipelines(CleanupStack& cleanup_stack); // Section 2

  /**
   * @brief Creates the main loop and initializes display/windows.
   *
   * Section 3: Create main loop and initialize display/windows.
   *
   * @param cleanup_stack Reference to a CleanupStack for registering cleanup tasks.
   * @return absl::Status indicating success or error.
   */
  absl::Status createMainLoop(CleanupStack& cleanup_stack); // Section 3

  /**
   * @brief Starts the pipelines and runs the main loop.
   *
   * Section 4: Set pipelines to PLAYING and run the main loop.
   *
   * @param cleanup_stack Reference to a CleanupStack for registering cleanup tasks.
   * @return absl::Status indicating success or error.
   */
  absl::Status playPipelines(CleanupStack& cleanup_stack); // Section 4

  //--------------------------------------------------------------------------
  // Callback and helper functions (mostly static, calling member functions)
  //--------------------------------------------------------------------------

  /**
   * @brief Callback invoked after bounding boxes have been generated.
   *
   * Processes metadata for each detected object.
   *
   * @param app_ctx Pointer to the application context.
   * @param buf Pointer to the GST buffer.
   * @param batch_meta Pointer to batch metadata.
   * @param index Index of the current instance.
   */
  static void all_bbox_generated(AppCtx* app_ctx, GstBuffer* buf, NvDsBatchMeta* batch_meta, guint index) {
    guint num_male = 0;
    guint num_female = 0;
    guint num_objects[128];
    memset(num_objects, 0, sizeof(num_objects));

    // Iterate over each frame metadata.
    for (NvDsMetaList* l_frame = batch_meta->frame_meta_list; l_frame != nullptr; l_frame = l_frame->next) {
      NvDsFrameMeta* frame_meta = reinterpret_cast<NvDsFrameMeta*>(l_frame->data);
      // Iterate over each object in the frame.
      for (NvDsMetaList* l_obj = frame_meta->obj_meta_list; l_obj != nullptr; l_obj = l_obj->next) {
        NvDsObjectMeta* obj = reinterpret_cast<NvDsObjectMeta*>(l_obj->data);
        // Check if the object is from the primary GIE.
        if (obj->unique_component_id == static_cast<gint>(app_ctx->config.primary_gie_config.unique_id)) {
          if (obj->class_id >= 0 && obj->class_id < 128)
            num_objects[obj->class_id]++;
          // For person class, adjust display text for gender.
          if (app_ctx->person_class_id > -1 && obj->class_id == app_ctx->person_class_id) {
            if (strstr(obj->text_params.display_text, "Man")) {
              str_replace(obj->text_params.display_text, "Man", "");
              str_replace(obj->text_params.display_text, "Person", "Man");
              num_male++;
            } else if (strstr(obj->text_params.display_text, "Woman")) {
              str_replace(obj->text_params.display_text, "Woman", "");
              str_replace(obj->text_params.display_text, "Person", "Woman");
              num_female++;
            }
          }
        }
      }
    }
  }

  /**
   * @brief Static signal interrupt handler.
   *
   * @param signum Signal number.
   */
  static void _intr_handler(int signum) {
    if (instance_)
      instance_->handle_intr(signum);
  }

  /**
   * @brief Handles an interrupt signal.
   *
   * Sets internal flag and resets signal handler to default.
   *
   * @param signum Signal number.
   */
  void handle_intr(int signum) {
    NVGSTDS_ERR_MSG_V("User Interrupted..\n");
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigaction(SIGINT, &action, nullptr);
    cintr_ = TRUE;
  }

  /**
   * @brief Sets up the interrupt signal handler.
   */
  void _intr_setup() {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = _intr_handler;
    sigaction(SIGINT, &action, nullptr);
  }

  /**
   * @brief Static performance callback.
   *
   * @param context Application context.
   * @param str Pointer to performance structure.
   */
  static void perf_cb_static(gpointer context, NvDsAppPerfStruct* str) {
    if (instance_)
      instance_->perf_cb(context, str);
  }

  /**
   * @brief Handles performance measurement callback.
   *
   * Updates FPS values and prints performance data.
   *
   * @param context Application context.
   * @param str Pointer to performance structure.
   */
  void perf_cb(gpointer context, NvDsAppPerfStruct* str) {
    static guint header_print_cnt = 0;
    guint i;
    AppCtx* app_ctx = reinterpret_cast<AppCtx*>(context);
    guint numf = str->num_instances;

    // Update FPS values under lock.
    g_mutex_lock(&fps_lock_);
    for (i = 0; i < numf; i++) {
      fps_[i] = str->fps[i];
      fps_avg_[i] = str->fps_avg[i];
    }

    // Print header periodically.
    if (header_print_cnt % 20 == 0) {
      g_print("\n**PERF:  ");
      for (i = 0; i < numf; i++) {
        g_print("FPS %d (Avg)\t", i);
      }
      g_print("\n");
      header_print_cnt = 0;
    }
    header_print_cnt++;
    if (num_instances_ > 1)
      g_print("PERF(%d): ", app_ctx->index);
    else
      g_print("**PERF:  ");
    for (i = 0; i < numf; i++) {
      g_print("%.2f (%.2f)\t", fps_[i], fps_avg_[i]);
    }
    g_print("\n");
    g_mutex_unlock(&fps_lock_);
  }

  /**
   * @brief Static callback to check for interrupt.
   *
   * @param data Unused.
   * @return gboolean TRUE if not interrupted, FALSE otherwise.
   */
  static gboolean check_for_interrupt_static(gpointer data) {
    if (instance_)
      return instance_->check_for_interrupt();
    return TRUE;
  }

  /**
   * @brief Checks for a user interrupt.
   *
   * If interrupted, quits the main loop.
   *
   * @return gboolean TRUE if continuing, FALSE if interrupt detected.
   */
  gboolean check_for_interrupt() {
    if (quit_)
      return FALSE;
    if (cintr_) {
      cintr_ = FALSE;
      quit_ = TRUE;
      if (main_loop_)
        g_main_loop_quit(main_loop_);
      return FALSE;
    }
    return TRUE;
  }

  /**
   * @brief Checks if a key has been pressed.
   *
   * @return gboolean TRUE if key pressed, FALSE otherwise.
   */
  static gboolean kbhit() {
    struct timeval tv;
    fd_set rdfs;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    FD_ZERO(&rdfs);
    FD_SET(STDIN_FILENO, &rdfs);
    select(STDIN_FILENO + 1, &rdfs, nullptr, nullptr, &tv);
    return FD_ISSET(STDIN_FILENO, &rdfs);
  }

  /**
   * @brief Changes the terminal mode.
   *
   * When enabling non-canonical mode, terminal input is processed character-by-character.
   *
   * @param dir 1 to enable non-canonical mode, otherwise restore original mode.
   */
  static void changemode(int dir) {
    static struct termios oldt, newt;
    if (dir == 1) {
      tcgetattr(STDIN_FILENO, &oldt);
      newt = oldt;
      newt.c_lflag &= ~(ICANON);
      tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    } else
      tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
  }

  /**
   * @brief Prints the runtime commands available to the user.
   */
  void print_runtime_commands() {
    g_print(
        "\nRuntime commands:\n"
        "\th: Print this help\n"
        "\tq: Quit\n\n"
        "\tp: Pause\n"
        "\tr: Resume\n\n");
    if (app_ctx_[0] && app_ctx_[0]->config.tiled_display_config.enable) {
      g_print(
          "NOTE: To expand a source in the 2D tiled display and view object details,\n"
          "      left-click on the source.\n"
          "      To go back to the tiled display, right-click anywhere on the window.\n\n");
    }
  }

  /**
   * @brief Static event thread function wrapper.
   *
   * @param arg Unused.
   * @return gboolean TRUE if event loop should continue.
   */
  static gboolean event_thread_func_static(gpointer arg) {
    if (instance_)
      return instance_->event_thread_func();
    return TRUE;
  }

  /**
   * @brief Processes user input and events.
   *
   * Handles keyboard and mouse events, updating the pipeline accordingly.
   *
   * @return gboolean TRUE if event loop should continue, FALSE otherwise.
   */
  gboolean event_thread_func() {
    guint i;
    gboolean ret = TRUE;

    // Check if all instances have quit.
    for (i = 0; i < num_instances_; i++) {
      if (app_ctx_[i] && !app_ctx_[i]->quit)
        break;
    }
    if (i == num_instances_) {
      quit_ = TRUE;
      if (main_loop_)
        g_main_loop_quit(main_loop_);
      return FALSE;
    }
    if (!kbhit())
      return TRUE;
    int c = fgetc(stdin);
    g_print("\n");

    gint source_id = -1;
    GstElement* tiler = (app_ctx_[rcfg_]) ? app_ctx_[rcfg_]->pipeline.tiled_display_bin.tiler : nullptr;
    // Process source selection if tiled display is enabled.
    if (app_ctx_[rcfg_] && app_ctx_[rcfg_]->config.tiled_display_config.enable && tiler) {
      g_object_get(G_OBJECT(tiler), "show-source", &source_id, nullptr);
      if (selecting_) {
        if (!rrowsel_) {
          if (c >= '0' && c <= '9') {
            rrow_ = c - '0';
            if (rrow_ < app_ctx_[rcfg_]->config.tiled_display_config.rows) {
              g_print("--selecting source  row %d--\n", rrow_);
              rrowsel_ = TRUE;
            } else {
              g_print("--selected source  row %d out of bound, reenter\n", rrow_);
            }
          }
        } else {
          if (c >= '0' && c <= '9') {
            unsigned int tile_num_columns = app_ctx_[rcfg_]->config.tiled_display_config.columns;
            rcol_ = c - '0';
            if (rcol_ < tile_num_columns) {
              selecting_ = FALSE;
              rrowsel_ = FALSE;
              source_id = tile_num_columns * rrow_ + rcol_;
              g_print("--selecting source  col %d sou=%d--\n", rcol_, source_id);
              if (source_id >= (gint)app_ctx_[rcfg_]->config.num_source_sub_bins)
                source_id = -1;
              else {
                app_ctx_[rcfg_]->show_bbox_text = TRUE;
                app_ctx_[rcfg_]->active_source_index = source_id;
                g_object_set(G_OBJECT(tiler), "show-source", source_id, nullptr);
              }
            } else {
              g_print("--selected source  col %d out of bound, reenter\n", rcol_);
            }
          }
        }
      }
    }
    // Process keyboard commands.
    switch (c) {
      case 'h':
        print_runtime_commands();
        break;
      case 'p':
        for (i = 0; i < num_instances_; i++)
          if (app_ctx_[i])
            pause_pipeline(app_ctx_[i].get());
        break;
      case 'r':
        for (i = 0; i < num_instances_; i++)
          if (app_ctx_[i])
            resume_pipeline(app_ctx_[i].get());
        break;
      case 'q':
        quit_ = TRUE;
        if (main_loop_)
          g_main_loop_quit(main_loop_);
        ret = FALSE;
        break;
      case 'c':
        // Allow selecting a different config file.
        if (app_ctx_[rcfg_] && app_ctx_[rcfg_]->config.tiled_display_config.enable && selecting_ == FALSE &&
            source_id == -1) {
          g_print("--selecting config file --\n");
          c = fgetc(stdin);
          if (c >= '0' && c <= '9') {
            rcfg_ = c - '0';
            if (rcfg_ < num_instances_)
              g_print("--selecting config  %d--\n", rcfg_);
            else {
              g_print("--selected config file %d out of bound, reenter\n", rcfg_);
              rcfg_ = 0;
            }
          }
        }
        break;
      case 'z':
        // Toggle source selection mode.
        if (app_ctx_[rcfg_] && app_ctx_[rcfg_]->config.tiled_display_config.enable && source_id == -1 &&
            selecting_ == FALSE) {
          g_print("--selecting source --\n");
          selecting_ = TRUE;
        } else {
          if (!show_bbox_text_ && app_ctx_[rcfg_])
            app_ctx_[rcfg_]->show_bbox_text = FALSE;
          if (tiler)
            g_object_set(G_OBJECT(tiler), "show-source", -1, nullptr);
          if (app_ctx_[rcfg_])
            app_ctx_[rcfg_]->active_source_index = -1;
          selecting_ = FALSE;
          rcfg_ = 0;
          g_print("--tiled mode --\n");
        }
        break;
      default:
        break;
    }
    return ret;
  }

  /**
   * @brief Computes the source id from normalized window coordinates.
   *
   * @param x_rel Relative x-coordinate (0 to 1).
   * @param y_rel Relative y-coordinate (0 to 1).
   * @param app_ctx Pointer to the application context.
   * @return int The computed source id, or -1 if out of bounds.
   */
  static int get_source_id_from_coordinates(float x_rel, float y_rel, AppCtx* app_ctx) {
    int tile_num_rows = app_ctx->config.tiled_display_config.rows;
    int tile_num_columns = app_ctx->config.tiled_display_config.columns;
    int source_id = static_cast<int>(x_rel * tile_num_columns);
    source_id += (static_cast<int>(y_rel * tile_num_rows)) * tile_num_columns;
    if (source_id >= (gint)app_ctx->config.num_source_sub_bins)
      source_id = -1;
    return source_id;
  }

  /**
   * @brief Static wrapper for the X event thread.
   *
   * @param data Unused.
   * @return gpointer Always returns nullptr.
   */
  static gpointer nvds_x_event_thread_static(gpointer data) {
    if (instance_)
      return instance_->nvds_x_event_thread();
    return nullptr;
  }

  /**
   * @brief Thread function to process X Window events.
   *
   * Monitors and processes X11 events like mouse clicks and key presses.
   *
   * @return gpointer Always returns nullptr.
   */
  gpointer nvds_x_event_thread() {
    g_mutex_lock(&disp_lock_);
    while (display_) {
      XEvent e;
      guint index;
      memset(&e, 0, sizeof(XEvent));
      // Process all pending X events.
      while (XPending(display_)) {
        XNextEvent(display_, &e);
        switch (e.type) {
          case ButtonPress: {
            XWindowAttributes win_attr;
            XButtonEvent ev = e.xbutton;
            gint source_id = -1;
            GstElement* tiler;
            memset(&win_attr, 0, sizeof(XWindowAttributes));
            XGetWindowAttributes(display_, ev.window, &win_attr);
            // Identify which window generated the event.
            for (index = 0; index < num_instances_; index++)
              if (ev.window == windows_[index])
                break;
            tiler = (index < num_instances_ && app_ctx_[index]) ? app_ctx_[index]->pipeline.tiled_display_bin.tiler
                                                                : nullptr;
            // Left mouse button: select a source.
            if (ev.button == Button1 && source_id == -1 && (index < num_instances_)) {
              if (app_ctx_[index])
                source_id = get_source_id_from_coordinates(
                    ev.x * 1.0f / win_attr.width, ev.y * 1.0f / win_attr.height, app_ctx_[index].get());
              else
                source_id = -1;
              if (source_id > -1 && tiler) {
                g_object_set(G_OBJECT(tiler), "show-source", source_id, nullptr);
                app_ctx_[index]->active_source_index = source_id;
                app_ctx_[index]->show_bbox_text = TRUE;
              }
            } else if (ev.button == Button3) { // Right mouse button: reset selection.
              if (tiler)
                g_object_set(G_OBJECT(tiler), "show-source", -1, nullptr);
              if (app_ctx_[index])
                app_ctx_[index]->active_source_index = -1;
            }
          } break;
          case KeyRelease:
          case KeyPress: {
            KeySym p, r, q;
            guint i;
            p = XKeysymToKeycode(display_, XK_P);
            r = XKeysymToKeycode(display_, XK_R);
            q = XKeysymToKeycode(display_, XK_Q);
            if (e.xkey.keycode == p) {
              for (i = 0; i < num_instances_; i++)
                if (app_ctx_[i])
                  pause_pipeline(app_ctx_[i].get());
              break;
            }
            if (e.xkey.keycode == r) {
              for (i = 0; i < num_instances_; i++)
                if (app_ctx_[i])
                  resume_pipeline(app_ctx_[i].get());
              break;
            }
            if (e.xkey.keycode == q) {
              quit_ = TRUE;
              if (main_loop_)
                g_main_loop_quit(main_loop_);
            }
          } break;
          case ClientMessage: {
            Atom wm_delete;
            // Determine which window sent the client message.
            for (index = 0; index < num_instances_; index++)
              if (e.xclient.window == windows_[index])
                break;
            wm_delete = XInternAtom(display_, "WM_DELETE_WINDOW", 1);
            if (wm_delete != None && wm_delete == (Atom)e.xclient.data.l[0]) {
              quit_ = TRUE;
              if (main_loop_)
                g_main_loop_quit(main_loop_);
            }
          } break;
        }
      }
      g_mutex_unlock(&disp_lock_);
      g_usleep(G_USEC_PER_SEC / 20);
      g_mutex_lock(&disp_lock_);
    }
    g_mutex_unlock(&disp_lock_);
    return nullptr;
  }

  /**
   * @brief Static wrapper for overlaying graphics.
   *
   * @param app_ctx Pointer to the application context.
   * @param buf Pointer to the GST buffer.
   * @param batch_meta Pointer to batch metadata.
   * @param index Index of the current instance.
   * @return gboolean TRUE if successful.
   */
  static gboolean overlay_graphics_static(AppCtx* app_ctx, GstBuffer* buf, NvDsBatchMeta* batch_meta, guint index) {
    return instance_ ? instance_->overlay_graphics(app_ctx, buf, batch_meta, index) : TRUE;
  }

  /**
   * @brief Overlays graphics (e.g., display text) on the video frame.
   *
   * Adds metadata for source information and latency.
   *
   * @param app_ctx Pointer to the application context.
   * @param buf Pointer to the GST buffer.
   * @param batch_meta Pointer to batch metadata.
   * @param index Index of the current instance.
   * @return gboolean TRUE if successful.
   */
  gboolean overlay_graphics(AppCtx* app_ctx, GstBuffer* buf, NvDsBatchMeta* batch_meta, guint index) {
    int src_index = app_ctx->active_source_index;
    if (src_index == -1)
      return TRUE;
    NvDsFrameLatencyInfo* latency_info = nullptr;
    // Acquire display meta data for overlay.
    NvDsDisplayMeta* display_meta = nvds_acquire_display_meta_from_pool(batch_meta);

    // Set primary text label with source URI.
    display_meta->num_labels = 1;
    display_meta->text_params[0].display_text =
        g_strdup_printf("Source: %s", app_ctx->config.multi_source_config[src_index].uri);
    display_meta->text_params[0].y_offset = 20;
    display_meta->text_params[0].x_offset = 20;
    display_meta->text_params[0].font_params.font_color = (NvOSD_ColorParams){0, 1, 0, 1};
    display_meta->text_params[0].font_params.font_size = app_ctx->config.osd_config.text_size * 1.5;
    display_meta->text_params[0].font_params.font_name = (char*)"Serif";
    display_meta->text_params[0].set_bg_clr = 1;
    display_meta->text_params[0].text_bg_clr = (NvOSD_ColorParams){0, 0, 0, 1.0};

    // If latency measurement is enabled, overlay latency information.
    if (nvds_enable_latency_measurement) {
      g_mutex_lock(&app_ctx->latency_lock);
      latency_info = &app_ctx->latency_info[index];
      display_meta->num_labels++;
      display_meta->text_params[1].display_text = g_strdup_printf("Latency: %lf", latency_info->latency);
      g_mutex_unlock(&app_ctx->latency_lock);
      display_meta->text_params[1].y_offset =
          (display_meta->text_params[0].y_offset * 2) + display_meta->text_params[0].font_params.font_size;
      display_meta->text_params[1].x_offset = 20;
      display_meta->text_params[1].font_params.font_color = (NvOSD_ColorParams){0, 1, 0, 1};
      display_meta->text_params[1].font_params.font_size = app_ctx->config.osd_config.text_size * 1.5;
      display_meta->text_params[1].font_params.font_name = (char*)"Arial";
      display_meta->text_params[1].set_bg_clr = 1;
      display_meta->text_params[1].text_bg_clr = (NvOSD_ColorParams){0, 0, 0, 1.0};
    }
    // Attach the display meta to the first frame.
    nvds_add_display_meta_to_frame(nvds_get_nth_frame_meta(batch_meta->frame_meta_list, 0), display_meta);
    return TRUE;
  }

  /**
   * @brief Static wrapper for the pipeline recreation thread.
   *
   * @param arg Pointer to the application context.
   * @return gboolean TRUE if successful.
   */
  static gboolean recreate_pipeline_thread_func_static(gpointer arg) {
    return instance_ ? instance_->recreate_pipeline_thread_func(arg) : FALSE;
  }

  /**
   * @brief Recreates the pipeline.
   *
   * Destroys and recreates the pipeline on a separate thread.
   *
   * @param arg Pointer to the application context.
   * @return gboolean TRUE if successful.
   */
  gboolean recreate_pipeline_thread_func(gpointer arg) {
    guint i;
    gboolean ret = TRUE;
    AppCtx* app_ctx_ptr = reinterpret_cast<AppCtx*>(arg);
    g_print("Destroy pipeline\n");
    destroy_pipeline(app_ctx_ptr);
    g_print("Recreate pipeline\n");
    if (!create_pipeline(app_ctx_ptr, nullptr, all_bbox_generated, perf_cb_static, overlay_graphics_static)) {
      NVGSTDS_ERR_MSG_V("Failed to create pipeline");
      return absl::InternalError("Failed to create pipeline").raw_code();
    }
    // Set the pipeline to PAUSED state before further configuration.
    if (gst_element_set_state(app_ctx_ptr->pipeline.pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
      NVGSTDS_ERR_MSG_V("Failed to set pipeline to PAUSED");
      return absl::InternalError("Failed to set pipeline to PAUSED").raw_code();
    }
    // Reset the window handles for video overlay.
    for (i = 0; i < app_ctx_ptr->config.num_sink_sub_bins; i++) {
      if (!GST_IS_VIDEO_OVERLAY(app_ctx_ptr->pipeline.instance_bins[0].sink_bin.sub_bins[i].sink))
        continue;
      gst_video_overlay_set_window_handle(
          GST_VIDEO_OVERLAY(app_ctx_ptr->pipeline.instance_bins[0].sink_bin.sub_bins[i].sink),
          (gulong)windows_[app_ctx_ptr->index]);
      gst_video_overlay_expose(GST_VIDEO_OVERLAY(app_ctx_ptr->pipeline.instance_bins[0].sink_bin.sub_bins[i].sink));
    }
    // Set the pipeline to PLAYING state.
    if (gst_element_set_state(app_ctx_ptr->pipeline.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      g_print("\ncan't set pipeline to playing state.\n");
      return absl::InternalError("can't set pipeline to playing state").raw_code();
    }
    return ret;
  }

 private:
  /// Vector of application context instances.
  std::vector<std::unique_ptr<HmApp>> app_ctx_;
  /// Flag set on SIGINT.
  guint cintr_;
  /// Main loop pointer.
  GMainLoop* main_loop_;
  /// Array of configuration file paths.
  gchar** cfg_files_;
  /// Array of input URI strings.
  gchar** input_uris_;
  /// Array of game id strings.
  gchar** game_id_;
  /// Flag to print version.
  gboolean print_version_;
  /// Flag to display bounding box text.
  gboolean show_bbox_text_;
  /// Flag to print dependency versions.
  gboolean print_dependencies_version_;
  /// Flag to quit the application.
  gboolean quit_;
  /// Flag to dump the pipeline dot file.
  gboolean dump_pipeline_dot_;
  /// Flag to force reconfiguration.
  gboolean force_reconfigure_;
  /// Return value for the application.
  gint return_value_;
  /// Number of pipeline instances.
  guint num_instances_;
  /// Number of input URIs.
  guint num_input_uris_;
  /// Mutex lock for FPS updates.
  GMutex fps_lock_;
  /// Array storing instantaneous FPS values.
  gdouble fps_[MAX_SOURCE_BINS];
  /// Array storing average FPS values.
  gdouble fps_avg_[MAX_SOURCE_BINS];
  /// X Display pointer.
  Display* display_;
  /// Vector of X Window handles.
  std::vector<Window> windows_;
  /// Thread for processing X events.
  GThread* x_event_thread_;
  /// Mutex lock for display access.
  GMutex disp_lock_;
  /// Row, column, and config selection variables.
  guint rrow_, rcol_, rcfg_;
  /// Flags for selection state.
  gboolean rrowsel_, selecting_;
  /// Name of the configuration file for stitching.
  static constexpr const char* configure_stitching_config_file_name_ = "ds_hockey_configure_stitching.yaml";

  /// Static pointer to the active PipelineApplication instance.
  static PipelineApplication* instance_;
};

//
// Define the static instance pointer
//
PipelineApplication* PipelineApplication::instance_ = nullptr;

//-----------------------------------------------------------------------------
// Definition of helper functions.
//-----------------------------------------------------------------------------

/**
 * @brief Initializes all HmApp instances with their configuration.
 *
 * Creates and configures each application instance by loading the appropriate config files.
 *
 * @param cleanup_stack Reference to a CleanupStack for registering cleanup tasks.
 * @return absl::Status indicating success or error.
 */
absl::Status PipelineApplication::initializeInstances(CleanupStack& /*cleanup_stack*/) {
  // Section 1: Create and initialize each HmApp instance.
  app_ctx_.resize(num_instances_);
  windows_.resize(num_instances_, 0);

  for (guint i = 0; i < num_instances_; i++) {
    // Create a new HmApp instance with game_id if available.
    app_ctx_[i] = std::make_unique<HmApp>(game_id_ ? *game_id_ : "");
    app_ctx_[i]->person_class_id = -1;
    app_ctx_[i]->car_class_id = -1;
    app_ctx_[i]->index = i;
    app_ctx_[i]->active_source_index = -1;
    if (show_bbox_text_)
      app_ctx_[i]->show_bbox_text = TRUE;
    // Set input URI if provided.
    if (input_uris_ && input_uris_[i]) {
      app_ctx_[i]->config.multi_source_config[0].uri = g_strdup_printf("%s", input_uris_[i]);
      g_free(input_uris_[i]);
    }
    // Load the base configuration.
    app_ctx_[i]->load_config();

    // Process YAML config file.
    if (g_str_has_suffix(cfg_files_[i], ".yml") || g_str_has_suffix(cfg_files_[i], ".yaml")) {
      if (!app_ctx_[i]->underlay_config("pipeline", cfg_files_[i])) {
        NVGSTDS_ERR_MSG_V("Failed to merge in config file '%s'", cfg_files_[i]);
        app_ctx_[i]->return_value = -1;
        return absl::InternalError("Failed to merge in config file");
      }
      HM_RETURN_IF_ERROR(app_ctx_[i]->complete_configuration(force_reconfigure_));
      YAML::Node config = app_ctx_[i]->configurator().config();
      if (!config["pipeline"].IsDefined() ||
          !parse_config_yaml(config["pipeline"], &app_ctx_[i]->config, cfg_files_[i])) {
        NVGSTDS_ERR_MSG_V("Failed to parse config file '%s'", cfg_files_[i]);
        app_ctx_[i]->return_value = -1;
        return absl::InternalError("Failed to parse config file");
      }
    } else if (g_str_has_suffix(cfg_files_[i], ".txt")) {
      // Process plain text config file.
      if (!parse_config_file(&app_ctx_[i]->config, cfg_files_[i])) {
        NVGSTDS_ERR_MSG_V("Failed to parse config file '%s'", cfg_files_[i]);
        app_ctx_[i]->return_value = -1;
        return absl::InternalError("Failed to parse config file");
      }
    }
  }
  return absl::OkStatus();
}

/**
 * @brief Creates pipelines for all instances.
 *
 * Iterates through each instance and creates its corresponding GStreamer pipeline.
 *
 * @param cleanup_stack Reference to a CleanupStack for registering cleanup tasks.
 * @return absl::Status indicating success or error.
 */
absl::Status PipelineApplication::createPipelines(CleanupStack& /*cleanup_stack*/) {
  // Section 2: Create pipelines for each instance.
  for (guint i = 0; i < num_instances_; i++) {
    if (!create_pipeline(app_ctx_[i].get(), nullptr, all_bbox_generated, perf_cb_static, overlay_graphics_static)) {
      NVGSTDS_ERR_MSG_V("Failed to create pipeline");
      return absl::InternalError("Failed to create pipeline");
    }
    // Optionally dump pipeline dot file for debugging.
    if (dump_pipeline_dot_) {
      std::string s = "pipeline";
      if (i) {
        s += '_';
        s += std::to_string(i);
      }
      gst_debug_bin_to_dot_file_with_ts(
          GST_BIN(app_ctx_[i]->pipeline.pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "/mnt/data/src/hstream/pipeline.dot");
    }
  }
  return absl::OkStatus();
}

/**
 * @brief Creates the main loop and initializes display windows.
 *
 * Sets up the X display, creates X windows for video output, and registers cleanup callbacks.
 *
 * @param cleanup_stack Reference to a CleanupStack for registering cleanup tasks.
 * @return absl::Status indicating success or error.
 */
absl::Status PipelineApplication::createMainLoop(CleanupStack& cleanup_stack) {
  // Section 3: Create main loop and initialize display/windows.
  main_loop_ = g_main_loop_new(nullptr, FALSE);
  // Register cleanup for main_loop_
  cleanup_stack.push([this] {
    if (main_loop_) {
      g_main_loop_unref(main_loop_);
      main_loop_ = nullptr;
    }
  });

  _intr_setup();
  g_timeout_add(400, check_for_interrupt_static, nullptr);

  g_mutex_init(&disp_lock_);
  display_ = XOpenDisplay(nullptr);
  if (!display_) {
    NVGSTDS_ERR_MSG_V("Could not open X Display");
    return absl::InternalError("Could not open X Display");
  }
  // Register cleanup for display_
  cleanup_stack.push([this] {
    g_mutex_lock(&disp_lock_);
    if (display_)
      XCloseDisplay(display_);
    display_ = nullptr;
    g_mutex_unlock(&disp_lock_);
  });

  // Create X windows and set pipeline window handles.
  for (guint i = 0; i < num_instances_; i++) {
#if defined(__aarch64__)
    // For aarch64 platforms, set pipeline to PAUSED before window creation.
    if (gst_element_set_state(app_ctx_[i]->pipeline.pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
      NVGSTDS_ERR_MSG_V("Failed to set pipeline to PAUSED");
      return absl::InternalError("Failed to set pipeline to PAUSED");
    }
#endif
    // Iterate over sink sub-bins to create and configure windows.
    for (guint j = 0; j < app_ctx_[i]->config.num_sink_sub_bins; j++) {
      if (!GST_IS_VIDEO_OVERLAY(app_ctx_[i]->pipeline.instance_bins[0].sink_bin.sub_bins[j].sink))
        continue;

      guint width = 0, height = 0;
      XSizeHints hints = {0};
      // Determine width and height from render config or tiled display config.
      if (app_ctx_[i]->config.sink_bin_sub_bin_config[j].render_config.width)
        width = app_ctx_[i]->config.sink_bin_sub_bin_config[j].render_config.width;
      else
        width = app_ctx_[i]->config.tiled_display_config.width;
      if (app_ctx_[i]->config.sink_bin_sub_bin_config[j].render_config.height)
        height = app_ctx_[i]->config.sink_bin_sub_bin_config[j].render_config.height;
      else
        height = app_ctx_[i]->config.tiled_display_config.height;
      width = (width) ? width : DEFAULT_X_WINDOW_WIDTH;
      height = (height) ? height : DEFAULT_X_WINDOW_HEIGHT;

      hints.flags = PPosition | PSize;
      hints.x = app_ctx_[i]->config.sink_bin_sub_bin_config[j].render_config.offset_x;
      hints.y = app_ctx_[i]->config.sink_bin_sub_bin_config[j].render_config.offset_y;
      hints.width = width;
      hints.height = height;

      // Create a simple X window.
      windows_[i] = XCreateSimpleWindow(
          display_,
          RootWindow(display_, DefaultScreen(display_)),
          hints.x,
          hints.y,
          width,
          height,
          2,
          0x00000000,
          0x00000000);

      XSetNormalHints(display_, windows_[i], &hints);

      // Set window title.
      gchar* title = (num_instances_ > 1) ? g_strdup_printf(APP_TITLE "-%d", i) : g_strdup(APP_TITLE);
      XTextProperty xproperty;
      if (XStringListToTextProperty((char**)&title, 1, &xproperty) != 0) {
        XSetWMName(display_, windows_[i], &xproperty);
        XFree(xproperty.value);
      }

      // Set event mask based on tiled display configuration.
      XSetWindowAttributes attr = {0};
      if ((app_ctx_[i]->config.tiled_display_config.enable &&
           app_ctx_[i]->config.tiled_display_config.rows * app_ctx_[i]->config.tiled_display_config.columns == 1) ||
          (app_ctx_[i]->config.tiled_display_config.enable == 0))
        attr.event_mask = KeyPress;
      else if (app_ctx_[i]->config.tiled_display_config.enable)
        attr.event_mask = ButtonPress | KeyRelease;
      XChangeWindowAttributes(display_, windows_[i], CWEventMask, &attr);

      // Set up window protocols for closing.
      Atom wmDeleteMessage = XInternAtom(display_, "WM_DELETE_WINDOW", False);
      if (wmDeleteMessage != None)
        XSetWMProtocols(display_, windows_[i], &wmDeleteMessage, 1);

      XMapRaised(display_, windows_[i]);
      XSync(display_, 1);
      // Set the window handle for video overlay.
      gst_video_overlay_set_window_handle(
          GST_VIDEO_OVERLAY(app_ctx_[i]->pipeline.instance_bins[0].sink_bin.sub_bins[j].sink), (gulong)windows_[i]);
      gst_video_overlay_expose(GST_VIDEO_OVERLAY(app_ctx_[i]->pipeline.instance_bins[0].sink_bin.sub_bins[j].sink));

      // Create X event thread if not already created.
      if (!x_event_thread_)
        x_event_thread_ = g_thread_new("nvds-window-event-thread", nvds_x_event_thread_static, nullptr);
    }
#if !defined(__aarch64__)
    // For non-aarch64 platforms, check if the GPU is integrated.
    int current_device = -1;
    cudaGetDevice(&current_device);
    struct cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, current_device);
    if (!prop.integrated) {
      if (gst_element_set_state(app_ctx_[i]->pipeline.pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
        NVGSTDS_ERR_MSG_V("Failed to set pipeline to PAUSED");
        return absl::InternalError("Failed to set pipeline to PAUSED");
      }
    }
#endif
  }
  // Register a cleanup that will destroy pipelines, windows, and display.
  cleanup_stack.push([this] {
    for (guint i = 0; i < num_instances_; i++) {
      if (app_ctx_[i]) {
        if (app_ctx_[i]->return_value == -1)
          return_value_ = -1;
        destroy_pipeline(app_ctx_[i].get());
        g_mutex_lock(&disp_lock_);
        if (windows_[i])
          XDestroyWindow(display_, windows_[i]);
        windows_[i] = 0;
        g_mutex_unlock(&disp_lock_);
        app_ctx_[i].reset();
      }
    }
    g_mutex_lock(&disp_lock_);
    if (display_)
      XCloseDisplay(display_);
    display_ = nullptr;
    g_mutex_unlock(&disp_lock_);
    g_mutex_clear(&disp_lock_);
  });

  return absl::OkStatus();
}

/**
 * @brief Sets all pipelines to PLAYING and starts the main loop.
 *
 * Applies post-configuration, sets pipelines to PLAYING state, and registers event handlers.
 *
 * @param cleanup_stack Reference to a CleanupStack for registering cleanup tasks.
 * @return absl::Status indicating success or error.
 */
absl::Status PipelineApplication::playPipelines(CleanupStack& /*cleanup_stack*/) {
  absl::Status status;
  // Set pipelines to PLAYING.
  for (guint i = 0; i < num_instances_; i++) {
    status = app_ctx_[i]->configurator().post_config_pipeline(app_ctx_[i]->pipeline, app_ctx_[i]->config);
    if (!status.ok()) {
      std::cerr << status << std::endl;
      g_print("\npipeline post-configuration failed.\n");
      return absl::InternalError("pipeline post-configuration failed");
    }
    if (gst_element_set_state(app_ctx_[i]->pipeline.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      g_print("\ncan't set pipeline to playing state.\n");
      return absl::InternalError("can't set pipeline to playing state");
    }
#if 1
    hm::save_dot_file(app_ctx_[i]->pipeline.pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "pipeline_running");
#endif
    if (app_ctx_[i]->config.pipeline_recreate_sec)
      g_timeout_add_seconds(
          app_ctx_[i]->config.pipeline_recreate_sec, recreate_pipeline_thread_func_static, app_ctx_[i].get());
  }

  print_runtime_commands();
  changemode(1);
  g_timeout_add(40, event_thread_func_static, nullptr);
  g_main_loop_run(main_loop_);
  changemode(0);

  // After the main loop exits, update status if any instance flagged an error.
  if (return_value_ != 0)
    status = absl::InternalError("App run failed");
  else
    g_print("App run successful\n");

  // When run() returns, cleanup_stack is destroyed (invoking the cleanups in reverse order).
  return status;
}

/**
 * @brief Main run function that orchestrates initialization and execution.
 *
 * Parses command-line arguments, initializes instances, creates pipelines,
 * sets up the main loop, and then starts pipeline playback.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return absl::Status indicating success or error.
 */
absl::Status PipelineApplication::run(int argc, char* argv[]) {
  absl::Status status = absl::OkStatus();
  GError* error = nullptr;

  // Declare our cleanup stack.
  CleanupStack cleanup_stack;

  // Build option entries.
  GOptionEntry entries[] = {
      {"version", 'v', 0, G_OPTION_ARG_NONE, &print_version_, "Print DeepStreamSDK version", nullptr},
      {"tiledtext", 't', 0, G_OPTION_ARG_NONE, &show_bbox_text_, "Display Bounding box labels in tiled mode", nullptr},
      {"dump-pipeline-dot",
       'd',
       0,
       G_OPTION_ARG_NONE,
       &dump_pipeline_dot_,
       "Dump graphviz dot file of pipeline",
       nullptr},
      {"version-all",
       0,
       0,
       G_OPTION_ARG_NONE,
       &print_dependencies_version_,
       "Print DeepStreamSDK and dependencies version",
       nullptr},
      {"cfg-file", 'c', 0, G_OPTION_ARG_FILENAME_ARRAY, &cfg_files_, "Set the config file", nullptr},
      {"game-id", 'g', 0, G_OPTION_ARG_FILENAME_ARRAY, &game_id_, "Game ID", nullptr},
      {"force-reconfigure", 'f', 0, G_OPTION_ARG_NONE, &force_reconfigure_, "Force reconfigure", nullptr},
      {"input-uri",
       'i',
       0,
       G_OPTION_ARG_FILENAME_ARRAY,
       &input_uris_,
       "Set the input uri (file://stream or rtsp://stream)",
       nullptr},
      {nullptr}};

  // Create and configure the option context.
  GOptionContext* ctx = g_option_context_new("Nvidia DeepStream Demo");
  auto cleanup_ctx = absl::MakeCleanup([ctx] {
    if (ctx) {
      g_option_context_free(ctx);
    }
  });
  GOptionGroup* group = g_option_group_new("abc", nullptr, nullptr, nullptr, nullptr);
  g_option_group_add_entries(group, entries);
  g_option_context_set_main_group(ctx, group);
  g_option_context_add_group(ctx, gst_init_get_option_group());

  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);

  // Initialize CUDA.
  int current_device = -1;
  cudaGetDevice(&current_device);
  struct cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, current_device);

  if (!g_option_context_parse(ctx, &argc, &argv, &error)) {
    NVGSTDS_ERR_MSG_V("%s", error->message);
    return absl::InternalError(error->message);
  }

  if (print_version_) {
    g_print(
        "deepstream-app version %d.%d.%d\n", NVDS_APP_VERSION_MAJOR, NVDS_APP_VERSION_MINOR, NVDS_APP_VERSION_MICRO);
    nvds_version_print();
    return status;
  }

  if (print_dependencies_version_) {
    g_print(
        "deepstream-app version %d.%d.%d\n", NVDS_APP_VERSION_MAJOR, NVDS_APP_VERSION_MINOR, NVDS_APP_VERSION_MICRO);
    nvds_version_print();
    nvds_dependencies_version_print();
    return status;
  }

  if (cfg_files_) {
    num_instances_ = g_strv_length(cfg_files_);
  }
  if (input_uris_) {
    num_input_uris_ = g_strv_length(input_uris_);
  }
  if (!cfg_files_ || num_instances_ == 0) {
    NVGSTDS_ERR_MSG_V("Specify config file with -c option");
    return absl::InternalError("Specify config file with -c option");
  }

  // Section 1: Create and initialize each HmApp instance.
  HM_RETURN_IF_ERROR(initializeInstances(cleanup_stack));

  // Section 2: Create pipelines for each instance.
  HM_RETURN_IF_ERROR(createPipelines(cleanup_stack));

  // Section 3: Create main loop and initialize display/windows.
  HM_RETURN_IF_ERROR(createMainLoop(cleanup_stack));

  HM_RETURN_IF_ERROR(playPipelines(cleanup_stack));

  // When run() returns, cleanup_stack is destroyed (invoking the cleanups in reverse order).
  return absl::OkStatus();
}

/**
 * @brief Main function.
 *
 * Creates a PipelineApplication instance, runs the application, and cleans up.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return int Exit status.
 */
int main(int argc, char* argv[]) {
  PipelineApplication app;
  absl::Status status = app.run(argc, argv);
  disable_perf_measurement();
  if (!status.ok()) {
    std::cerr << status << std::endl;
    return status.raw_code();
  }
  return 0;
}
