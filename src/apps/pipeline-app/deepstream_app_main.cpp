/* clang-format off */
// X11 stuff must come first because it defined "Status"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#undef Status
/* clang-format on */

#include "hstream/src/apps/apps-common/deepstream_app_version.h"
#include "hstream/src/apps/apps-common/deepstream_common.h"
// #include "hstream/src/apps/apps-common/deepstream_config_file_parser.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/pipeline_utils.h"

#include <cuda_runtime_api.h>
#include <gst/gstbin.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
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

//
// PipelineApplication encapsulates all global state and callback functions.
//
class PipelineApplication {
 public:
  PipelineApplication() {
    cintr = FALSE;
    main_loop = nullptr;
    cfg_files = nullptr;
    input_uris = nullptr;
    game_id = nullptr;
    print_version = FALSE;
    show_bbox_text = FALSE;
    print_dependencies_version = FALSE;
    quit = FALSE;
    dump_pipeline_dot = FALSE;
    force_reconfigure = FALSE;
    return_value = 0;
    num_instances = 0;
    num_input_uris = 0;
    memset(fps, 0, sizeof(fps));
    memset(fps_avg, 0, sizeof(fps_avg));
    display = nullptr;
    // Vectors are default–initialized.
    x_event_thread = nullptr;
    rrow = rcol = rcfg = 0;
    rrowsel = FALSE;
    selecting = FALSE;
    g_mutex_init(&fps_lock);
    g_mutex_init(&disp_lock);
    instance_ = this;
  }

  ~PipelineApplication() {
    g_mutex_clear(&disp_lock);
  }

  // Main run function. Cleanup objects will be declared in run().
  absl::Status run(int argc, char* argv[]);

  // Helper functions that separate key sections.
  absl::Status initializeInstances(); // Section 1
  absl::Status createPipelines(); // Section 2
  absl::Status createMainLoop(); // Section 3

  //--------------------------------------------------------------------------
  // Callback and helper functions (mostly static, calling member functions)
  //--------------------------------------------------------------------------

  static void all_bbox_generated(AppCtx* appCtx, GstBuffer* buf, NvDsBatchMeta* batch_meta, guint index) {
    guint num_male = 0;
    guint num_female = 0;
    guint num_objects[128];
    memset(num_objects, 0, sizeof(num_objects));

    for (NvDsMetaList* l_frame = batch_meta->frame_meta_list; l_frame != nullptr; l_frame = l_frame->next) {
      NvDsFrameMeta* frame_meta = reinterpret_cast<NvDsFrameMeta*>(l_frame->data);
      for (NvDsMetaList* l_obj = frame_meta->obj_meta_list; l_obj != nullptr; l_obj = l_obj->next) {
        NvDsObjectMeta* obj = reinterpret_cast<NvDsObjectMeta*>(l_obj->data);
        if (obj->unique_component_id == static_cast<gint>(appCtx->config.primary_gie_config.unique_id)) {
          if (obj->class_id >= 0 && obj->class_id < 128)
            num_objects[obj->class_id]++;
          if (appCtx->person_class_id > -1 && obj->class_id == appCtx->person_class_id) {
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

  static void _intr_handler(int signum) {
    if (instance_)
      instance_->handle_intr(signum);
  }
  void handle_intr(int signum) {
    NVGSTDS_ERR_MSG_V("User Interrupted..\n");
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigaction(SIGINT, &action, nullptr);
    cintr = TRUE;
  }
  void _intr_setup() {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = _intr_handler;
    sigaction(SIGINT, &action, nullptr);
  }

  static void perf_cb_static(gpointer context, NvDsAppPerfStruct* str) {
    if (instance_)
      instance_->perf_cb(context, str);
  }
  void perf_cb(gpointer context, NvDsAppPerfStruct* str) {
    static guint header_print_cnt = 0;
    guint i;
    AppCtx* appCtx = reinterpret_cast<AppCtx*>(context);
    guint numf = str->num_instances;

    g_mutex_lock(&fps_lock);
    for (i = 0; i < numf; i++) {
      fps[i] = str->fps[i];
      fps_avg[i] = str->fps_avg[i];
    }

    if (header_print_cnt % 20 == 0) {
      g_print("\n**PERF:  ");
      for (i = 0; i < numf; i++) {
        g_print("FPS %d (Avg)\t", i);
      }
      g_print("\n");
      header_print_cnt = 0;
    }
    header_print_cnt++;
    if (num_instances > 1)
      g_print("PERF(%d): ", appCtx->index);
    else
      g_print("**PERF:  ");
    for (i = 0; i < numf; i++) {
      g_print("%.2f (%.2f)\t", fps[i], fps_avg[i]);
    }
    g_print("\n");
    g_mutex_unlock(&fps_lock);
  }

  static gboolean check_for_interrupt_static(gpointer data) {
    if (instance_)
      return instance_->check_for_interrupt();
    return TRUE;
  }
  gboolean check_for_interrupt() {
    if (quit)
      return FALSE;
    if (cintr) {
      cintr = FALSE;
      quit = TRUE;
      if (main_loop)
        g_main_loop_quit(main_loop);
      return FALSE;
    }
    return TRUE;
  }

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

  void print_runtime_commands() {
    g_print(
        "\nRuntime commands:\n"
        "\th: Print this help\n"
        "\tq: Quit\n\n"
        "\tp: Pause\n"
        "\tr: Resume\n\n");
    if (appCtx[0] && appCtx[0]->config.tiled_display_config.enable) {
      g_print(
          "NOTE: To expand a source in the 2D tiled display and view object details,\n"
          "      left-click on the source.\n"
          "      To go back to the tiled display, right-click anywhere on the window.\n\n");
    }
  }

  static gboolean event_thread_func_static(gpointer arg) {
    if (instance_)
      return instance_->event_thread_func();
    return TRUE;
  }
  gboolean event_thread_func() {
    guint i;
    gboolean ret = TRUE;

    // Check if all instances have quit.
    for (i = 0; i < num_instances; i++) {
      if (appCtx[i] && !appCtx[i]->quit)
        break;
    }
    if (i == num_instances) {
      quit = TRUE;
      if (main_loop)
        g_main_loop_quit(main_loop);
      return FALSE;
    }
    if (!kbhit())
      return TRUE;
    int c = fgetc(stdin);
    g_print("\n");

    gint source_id = -1;
    GstElement* tiler = (appCtx[rcfg]) ? appCtx[rcfg]->pipeline.tiled_display_bin.tiler : nullptr;
    if (appCtx[rcfg] && appCtx[rcfg]->config.tiled_display_config.enable && tiler) {
      g_object_get(G_OBJECT(tiler), "show-source", &source_id, nullptr);
      if (selecting) {
        if (!rrowsel) {
          if (c >= '0' && c <= '9') {
            rrow = c - '0';
            if (rrow < appCtx[rcfg]->config.tiled_display_config.rows) {
              g_print("--selecting source  row %d--\n", rrow);
              rrowsel = TRUE;
            } else {
              g_print("--selected source  row %d out of bound, reenter\n", rrow);
            }
          }
        } else {
          if (c >= '0' && c <= '9') {
            unsigned int tile_num_columns = appCtx[rcfg]->config.tiled_display_config.columns;
            rcol = c - '0';
            if (rcol < tile_num_columns) {
              selecting = FALSE;
              rrowsel = FALSE;
              source_id = tile_num_columns * rrow + rcol;
              g_print("--selecting source  col %d sou=%d--\n", rcol, source_id);
              if (source_id >= (gint)appCtx[rcfg]->config.num_source_sub_bins)
                source_id = -1;
              else {
                appCtx[rcfg]->show_bbox_text = TRUE;
                appCtx[rcfg]->active_source_index = source_id;
                g_object_set(G_OBJECT(tiler), "show-source", source_id, nullptr);
              }
            } else {
              g_print("--selected source  col %d out of bound, reenter\n", rcol);
            }
          }
        }
      }
    }
    switch (c) {
      case 'h':
        print_runtime_commands();
        break;
      case 'p':
        for (i = 0; i < num_instances; i++)
          if (appCtx[i])
            pause_pipeline(appCtx[i].get());
        break;
      case 'r':
        for (i = 0; i < num_instances; i++)
          if (appCtx[i])
            resume_pipeline(appCtx[i].get());
        break;
      case 'q':
        quit = TRUE;
        if (main_loop)
          g_main_loop_quit(main_loop);
        ret = FALSE;
        break;
      case 'c':
        if (appCtx[rcfg] && appCtx[rcfg]->config.tiled_display_config.enable && selecting == FALSE && source_id == -1) {
          g_print("--selecting config file --\n");
          c = fgetc(stdin);
          if (c >= '0' && c <= '9') {
            rcfg = c - '0';
            if (rcfg < num_instances)
              g_print("--selecting config  %d--\n", rcfg);
            else {
              g_print("--selected config file %d out of bound, reenter\n", rcfg);
              rcfg = 0;
            }
          }
        }
        break;
      case 'z':
        if (appCtx[rcfg] && appCtx[rcfg]->config.tiled_display_config.enable && source_id == -1 && selecting == FALSE) {
          g_print("--selecting source --\n");
          selecting = TRUE;
        } else {
          if (!show_bbox_text && appCtx[rcfg])
            appCtx[rcfg]->show_bbox_text = FALSE;
          if (tiler)
            g_object_set(G_OBJECT(tiler), "show-source", -1, nullptr);
          if (appCtx[rcfg])
            appCtx[rcfg]->active_source_index = -1;
          selecting = FALSE;
          rcfg = 0;
          g_print("--tiled mode --\n");
        }
        break;
      default:
        break;
    }
    return ret;
  }

  static int get_source_id_from_coordinates(float x_rel, float y_rel, AppCtx* appCtx) {
    int tile_num_rows = appCtx->config.tiled_display_config.rows;
    int tile_num_columns = appCtx->config.tiled_display_config.columns;
    int source_id = static_cast<int>(x_rel * tile_num_columns);
    source_id += (static_cast<int>(y_rel * tile_num_rows)) * tile_num_columns;
    if (source_id >= (gint)appCtx->config.num_source_sub_bins)
      source_id = -1;
    return source_id;
  }

  static gpointer nvds_x_event_thread_static(gpointer data) {
    if (instance_)
      return instance_->nvds_x_event_thread();
    return nullptr;
  }
  gpointer nvds_x_event_thread() {
    g_mutex_lock(&disp_lock);
    while (display) {
      XEvent e;
      guint index;
      memset(&e, 0, sizeof(XEvent));
      while (XPending(display)) {
        XNextEvent(display, &e);
        switch (e.type) {
          case ButtonPress: {
            XWindowAttributes win_attr;
            XButtonEvent ev = e.xbutton;
            gint source_id = -1;
            GstElement* tiler;
            memset(&win_attr, 0, sizeof(XWindowAttributes));
            XGetWindowAttributes(display, ev.window, &win_attr);
            for (index = 0; index < num_instances; index++)
              if (ev.window == windows[index])
                break;
            tiler =
                (index < num_instances && appCtx[index]) ? appCtx[index]->pipeline.tiled_display_bin.tiler : nullptr;
            if (ev.button == Button1 && source_id == -1 && (index < num_instances)) {
              if (appCtx[index])
                source_id = get_source_id_from_coordinates(
                    ev.x * 1.0f / win_attr.width, ev.y * 1.0f / win_attr.height, appCtx[index].get());
              else
                source_id = -1;
              if (source_id > -1 && tiler) {
                g_object_set(G_OBJECT(tiler), "show-source", source_id, nullptr);
                appCtx[index]->active_source_index = source_id;
                appCtx[index]->show_bbox_text = TRUE;
              }
            } else if (ev.button == Button3) {
              if (tiler)
                g_object_set(G_OBJECT(tiler), "show-source", -1, nullptr);
              if (appCtx[index])
                appCtx[index]->active_source_index = -1;
            }
          } break;
          case KeyRelease:
          case KeyPress: {
            KeySym p, r, q;
            guint i;
            p = XKeysymToKeycode(display, XK_P);
            r = XKeysymToKeycode(display, XK_R);
            q = XKeysymToKeycode(display, XK_Q);
            if (e.xkey.keycode == p) {
              for (i = 0; i < num_instances; i++)
                if (appCtx[i])
                  pause_pipeline(appCtx[i].get());
              break;
            }
            if (e.xkey.keycode == r) {
              for (i = 0; i < num_instances; i++)
                if (appCtx[i])
                  resume_pipeline(appCtx[i].get());
              break;
            }
            if (e.xkey.keycode == q) {
              quit = TRUE;
              if (main_loop)
                g_main_loop_quit(main_loop);
            }
          } break;
          case ClientMessage: {
            Atom wm_delete;
            for (index = 0; index < num_instances; index++)
              if (e.xclient.window == windows[index])
                break;
            wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", 1);
            if (wm_delete != None && wm_delete == (Atom)e.xclient.data.l[0]) {
              quit = TRUE;
              if (main_loop)
                g_main_loop_quit(main_loop);
            }
          } break;
        }
      }
      g_mutex_unlock(&disp_lock);
      g_usleep(G_USEC_PER_SEC / 20);
      g_mutex_lock(&disp_lock);
    }
    g_mutex_unlock(&disp_lock);
    return nullptr;
  }

  static gboolean overlay_graphics_static(AppCtx* appCtx, GstBuffer* buf, NvDsBatchMeta* batch_meta, guint index) {
    return instance_ ? instance_->overlay_graphics(appCtx, buf, batch_meta, index) : TRUE;
  }
  gboolean overlay_graphics(AppCtx* appCtx, GstBuffer* buf, NvDsBatchMeta* batch_meta, guint index) {
    int srcIndex = appCtx->active_source_index;
    if (srcIndex == -1)
      return TRUE;
    NvDsFrameLatencyInfo* latency_info = nullptr;
    NvDsDisplayMeta* display_meta = nvds_acquire_display_meta_from_pool(batch_meta);

    display_meta->num_labels = 1;
    display_meta->text_params[0].display_text =
        g_strdup_printf("Source: %s", appCtx->config.multi_source_config[srcIndex].uri);
    display_meta->text_params[0].y_offset = 20;
    display_meta->text_params[0].x_offset = 20;
    display_meta->text_params[0].font_params.font_color = (NvOSD_ColorParams){0, 1, 0, 1};
    display_meta->text_params[0].font_params.font_size = appCtx->config.osd_config.text_size * 1.5;
    display_meta->text_params[0].font_params.font_name = (char*)"Serif";
    display_meta->text_params[0].set_bg_clr = 1;
    display_meta->text_params[0].text_bg_clr = (NvOSD_ColorParams){0, 0, 0, 1.0};

    if (nvds_enable_latency_measurement) {
      g_mutex_lock(&appCtx->latency_lock);
      latency_info = &appCtx->latency_info[index];
      display_meta->num_labels++;
      display_meta->text_params[1].display_text = g_strdup_printf("Latency: %lf", latency_info->latency);
      g_mutex_unlock(&appCtx->latency_lock);
      display_meta->text_params[1].y_offset =
          (display_meta->text_params[0].y_offset * 2) + display_meta->text_params[0].font_params.font_size;
      display_meta->text_params[1].x_offset = 20;
      display_meta->text_params[1].font_params.font_color = (NvOSD_ColorParams){0, 1, 0, 1};
      display_meta->text_params[1].font_params.font_size = appCtx->config.osd_config.text_size * 1.5;
      display_meta->text_params[1].font_params.font_name = (char*)"Arial";
      display_meta->text_params[1].set_bg_clr = 1;
      display_meta->text_params[1].text_bg_clr = (NvOSD_ColorParams){0, 0, 0, 1.0};
    }
    nvds_add_display_meta_to_frame(nvds_get_nth_frame_meta(batch_meta->frame_meta_list, 0), display_meta);
    return TRUE;
  }

  static gboolean recreate_pipeline_thread_func_static(gpointer arg) {
    return instance_ ? instance_->recreate_pipeline_thread_func(arg) : FALSE;
  }
  gboolean recreate_pipeline_thread_func(gpointer arg) {
    guint i;
    gboolean ret = TRUE;
    AppCtx* appCtx_ptr = reinterpret_cast<AppCtx*>(arg);
    g_print("Destroy pipeline\n");
    destroy_pipeline(appCtx_ptr);
    g_print("Recreate pipeline\n");
    if (!create_pipeline(appCtx_ptr, nullptr, all_bbox_generated, perf_cb_static, overlay_graphics_static)) {
      NVGSTDS_ERR_MSG_V("Failed to create pipeline");
      return absl::InternalError("Failed to create pipeline").raw_code();
    }
    if (gst_element_set_state(appCtx_ptr->pipeline.pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
      NVGSTDS_ERR_MSG_V("Failed to set pipeline to PAUSED");
      return absl::InternalError("Failed to set pipeline to PAUSED").raw_code();
    }
    for (i = 0; i < appCtx_ptr->config.num_sink_sub_bins; i++) {
      if (!GST_IS_VIDEO_OVERLAY(appCtx_ptr->pipeline.instance_bins[0].sink_bin.sub_bins[i].sink))
        continue;
      gst_video_overlay_set_window_handle(
          GST_VIDEO_OVERLAY(appCtx_ptr->pipeline.instance_bins[0].sink_bin.sub_bins[i].sink),
          (gulong)windows[appCtx_ptr->index]);
      gst_video_overlay_expose(GST_VIDEO_OVERLAY(appCtx_ptr->pipeline.instance_bins[0].sink_bin.sub_bins[i].sink));
    }
    if (gst_element_set_state(appCtx_ptr->pipeline.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      g_print("\ncan't set pipeline to playing state.\n");
      return absl::InternalError("can't set pipeline to playing state").raw_code();
    }
    return ret;
  }

 private:
  // Member variables.
  std::vector<std::unique_ptr<HmApp>> appCtx;
  guint cintr;
  GMainLoop* main_loop;
  gchar** cfg_files;
  gchar** input_uris;
  gchar** game_id;
  gboolean print_version;
  gboolean show_bbox_text;
  gboolean print_dependencies_version;
  gboolean quit;
  gboolean dump_pipeline_dot;
  gboolean force_reconfigure;
  gint return_value;
  guint num_instances;
  guint num_input_uris;
  GMutex fps_lock;
  gdouble fps[MAX_SOURCE_BINS];
  gdouble fps_avg[MAX_SOURCE_BINS];
  Display* display;
  std::vector<Window> windows;
  GThread* x_event_thread;
  GMutex disp_lock;
  guint rrow, rcol, rcfg;
  gboolean rrowsel, selecting;
  static constexpr const char* kConfigureStitchingConfigFileName = "ds_hockey_configure_stitching.yaml";

  // A static pointer to the one active PipelineApplication instance.
  static PipelineApplication* instance_;
};

//
// Define the static instance pointer
//
PipelineApplication* PipelineApplication::instance_ = nullptr;

//-----------------------------------------------------------------------------
// Definition of helper functions.
//-----------------------------------------------------------------------------

absl::Status PipelineApplication::initializeInstances() {
  // Section 1: Create and initialize each HmApp instance.
  appCtx.resize(num_instances);
  windows.resize(num_instances, 0);

  for (guint i = 0; i < num_instances; i++) {
    appCtx[i] = std::make_unique<HmApp>(game_id ? *game_id : "");
    appCtx[i]->person_class_id = -1;
    appCtx[i]->car_class_id = -1;
    appCtx[i]->index = i;
    appCtx[i]->active_source_index = -1;
    if (show_bbox_text)
      appCtx[i]->show_bbox_text = TRUE;
    if (input_uris && input_uris[i]) {
      appCtx[i]->config.multi_source_config[0].uri = g_strdup_printf("%s", input_uris[i]);
      g_free(input_uris[i]);
    }
    appCtx[i]->load_config();

    if (g_str_has_suffix(cfg_files[i], ".yml") || g_str_has_suffix(cfg_files[i], ".yaml")) {
      if (!appCtx[i]->underlay_config("pipeline", cfg_files[i])) {
        NVGSTDS_ERR_MSG_V("Failed to merge in config file '%s'", cfg_files[i]);
        appCtx[i]->return_value = -1;
        return absl::InternalError("Failed to merge in config file");
      }
      HM_RETURN_IF_ERROR(appCtx[i]->complete_configuration(force_reconfigure));
      YAML::Node config = appCtx[i]->configurator().config();
      if (!config["pipeline"].IsDefined() || !parse_config_yaml(config["pipeline"], &appCtx[i]->config, cfg_files[i])) {
        NVGSTDS_ERR_MSG_V("Failed to parse config file '%s'", cfg_files[i]);
        appCtx[i]->return_value = -1;
        return absl::InternalError("Failed to parse config file");
      }
    } else if (g_str_has_suffix(cfg_files[i], ".txt")) {
      if (!parse_config_file(&appCtx[i]->config, cfg_files[i])) {
        NVGSTDS_ERR_MSG_V("Failed to parse config file '%s'", cfg_files[i]);
        appCtx[i]->return_value = -1;
        return absl::InternalError("Failed to parse config file");
      }
    }
  }
  return absl::OkStatus();
}

absl::Status PipelineApplication::createPipelines() {
  // Section 2: Create pipelines for each instance.
  for (guint i = 0; i < num_instances; i++) {
    if (!create_pipeline(appCtx[i].get(), nullptr, all_bbox_generated, perf_cb_static, overlay_graphics_static)) {
      NVGSTDS_ERR_MSG_V("Failed to create pipeline");
      return absl::InternalError("Failed to create pipeline");
    }
    if (dump_pipeline_dot) {
      std::string s = "pipeline";
      if (i) {
        s += '_';
        s += std::to_string(i);
      }
      gst_debug_bin_to_dot_file_with_ts(
          GST_BIN(appCtx[i]->pipeline.pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "/mnt/data/src/hstream/pipeline.dot");
    }
  }
  return absl::OkStatus();
}

absl::Status PipelineApplication::createMainLoop() {
  // Section 3: Create main loop and initialize display.
  main_loop = g_main_loop_new(nullptr, FALSE);
  _intr_setup();
  g_timeout_add(400, check_for_interrupt_static, nullptr);

  g_mutex_init(&disp_lock);
  display = XOpenDisplay(nullptr);
  if (!display) {
    NVGSTDS_ERR_MSG_V("Could not open X Display");
    return absl::InternalError("Could not open X Display");
  }

  // Create X windows and set the pipeline window handles.
  // (Note: We do not create any cleanup objects here; cleanup is deferred to run().)
  for (guint i = 0; i < num_instances; i++) {
#if defined(__aarch64__)
    if (gst_element_set_state(appCtx[i]->pipeline.pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
      NVGSTDS_ERR_MSG_V("Failed to set pipeline to PAUSED");
      return absl::InternalError("Failed to set pipeline to PAUSED");
    }
#endif
    for (guint j = 0; j < appCtx[i]->config.num_sink_sub_bins; j++) {
      if (!GST_IS_VIDEO_OVERLAY(appCtx[i]->pipeline.instance_bins[0].sink_bin.sub_bins[j].sink))
        continue;

      guint width = 0, height = 0;
      XSizeHints hints = {0};
      if (appCtx[i]->config.sink_bin_sub_bin_config[j].render_config.width)
        width = appCtx[i]->config.sink_bin_sub_bin_config[j].render_config.width;
      else
        width = appCtx[i]->config.tiled_display_config.width;
      if (appCtx[i]->config.sink_bin_sub_bin_config[j].render_config.height)
        height = appCtx[i]->config.sink_bin_sub_bin_config[j].render_config.height;
      else
        height = appCtx[i]->config.tiled_display_config.height;
      width = (width) ? width : DEFAULT_X_WINDOW_WIDTH;
      height = (height) ? height : DEFAULT_X_WINDOW_HEIGHT;

      hints.flags = PPosition | PSize;
      hints.x = appCtx[i]->config.sink_bin_sub_bin_config[j].render_config.offset_x;
      hints.y = appCtx[i]->config.sink_bin_sub_bin_config[j].render_config.offset_y;
      hints.width = width;
      hints.height = height;

      windows[i] = XCreateSimpleWindow(
          display,
          RootWindow(display, DefaultScreen(display)),
          hints.x,
          hints.y,
          width,
          height,
          2,
          0x00000000,
          0x00000000);

      XSetNormalHints(display, windows[i], &hints);

      gchar* title = (num_instances > 1) ? g_strdup_printf(APP_TITLE "-%d", i) : g_strdup(APP_TITLE);
      XTextProperty xproperty;
      if (XStringListToTextProperty((char**)&title, 1, &xproperty) != 0) {
        XSetWMName(display, windows[i], &xproperty);
        XFree(xproperty.value);
      }

      XSetWindowAttributes attr = {0};
      if ((appCtx[i]->config.tiled_display_config.enable &&
           appCtx[i]->config.tiled_display_config.rows * appCtx[i]->config.tiled_display_config.columns == 1) ||
          (appCtx[i]->config.tiled_display_config.enable == 0))
        attr.event_mask = KeyPress;
      else if (appCtx[i]->config.tiled_display_config.enable)
        attr.event_mask = ButtonPress | KeyRelease;
      XChangeWindowAttributes(display, windows[i], CWEventMask, &attr);

      Atom wmDeleteMessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
      if (wmDeleteMessage != None)
        XSetWMProtocols(display, windows[i], &wmDeleteMessage, 1);

      XMapRaised(display, windows[i]);
      XSync(display, 1);
      gst_video_overlay_set_window_handle(
          GST_VIDEO_OVERLAY(appCtx[i]->pipeline.instance_bins[0].sink_bin.sub_bins[j].sink), (gulong)windows[i]);
      gst_video_overlay_expose(GST_VIDEO_OVERLAY(appCtx[i]->pipeline.instance_bins[0].sink_bin.sub_bins[j].sink));

      if (!x_event_thread)
        x_event_thread = g_thread_new("nvds-window-event-thread", nvds_x_event_thread_static, nullptr);
    }
#if !defined(__aarch64__)
    // For non-aarch64 platforms, check integrated GPU flag.
    int current_device = -1;
    cudaGetDevice(&current_device);
    struct cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, current_device);
    if (!prop.integrated) {
      if (gst_element_set_state(appCtx[i]->pipeline.pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
        NVGSTDS_ERR_MSG_V("Failed to set pipeline to PAUSED");
        return absl::InternalError("Failed to set pipeline to PAUSED");
      }
    }
#endif
  }
  return absl::OkStatus();
}

//-----------------------------------------------------------------------------
// Main run function definition.
//-----------------------------------------------------------------------------
absl::Status PipelineApplication::run(int argc, char* argv[]) {
  absl::Status status = absl::OkStatus();
  GError* error = nullptr;

  // Build option entries.
  GOptionEntry entries[] = {
      {"version", 'v', 0, G_OPTION_ARG_NONE, &print_version, "Print DeepStreamSDK version", nullptr},
      {"tiledtext", 't', 0, G_OPTION_ARG_NONE, &show_bbox_text, "Display Bounding box labels in tiled mode", nullptr},
      {"dump-pipeline-dot",
       'd',
       0,
       G_OPTION_ARG_NONE,
       &dump_pipeline_dot,
       "Dump graphviz dot file of pipeline",
       nullptr},
      {"version-all",
       0,
       0,
       G_OPTION_ARG_NONE,
       &print_dependencies_version,
       "Print DeepStreamSDK and dependencies version",
       nullptr},
      {"cfg-file", 'c', 0, G_OPTION_ARG_FILENAME_ARRAY, &cfg_files, "Set the config file", nullptr},
      {"game-id", 'g', 0, G_OPTION_ARG_FILENAME_ARRAY, &game_id, "Game ID", nullptr},
      {"force-reconfigure", 'f', 0, G_OPTION_ARG_NONE, &force_reconfigure, "Force reconfigure", nullptr},
      {"input-uri",
       'i',
       0,
       G_OPTION_ARG_FILENAME_ARRAY,
       &input_uris,
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

  if (print_version) {
    g_print(
        "deepstream-app version %d.%d.%d\n", NVDS_APP_VERSION_MAJOR, NVDS_APP_VERSION_MINOR, NVDS_APP_VERSION_MICRO);
    nvds_version_print();
    return status;
  }

  if (print_dependencies_version) {
    g_print(
        "deepstream-app version %d.%d.%d\n", NVDS_APP_VERSION_MAJOR, NVDS_APP_VERSION_MINOR, NVDS_APP_VERSION_MICRO);
    nvds_version_print();
    nvds_dependencies_version_print();
    return status;
  }

  if (cfg_files) {
    num_instances = g_strv_length(cfg_files);
  }
  if (input_uris) {
    num_input_uris = g_strv_length(input_uris);
  }
  if (!cfg_files || num_instances == 0) {
    NVGSTDS_ERR_MSG_V("Specify config file with -c option");
    return absl::InternalError("Specify config file with -c option");
  }

  // Section 1: Create and initialize each HmApp instance.
  status = initializeInstances();
  if (!status.ok()) {
    return status;
  }

  // Section 2: Create pipelines for each instance.
  status = createPipelines();
  if (!status.ok()) {
    return status;
  }

  // Section 3: Create main loop and initialize display/windows.
  status = createMainLoop();
  if (!status.ok()) {
    return status;
  }

  // --- RAII cleanup objects declared in the run() scope ---
  auto cleanup_main_loop = absl::MakeCleanup([this] {
    if (main_loop)
      g_main_loop_unref(main_loop);
    main_loop = nullptr;
  });
  auto cleanup_display = absl::MakeCleanup([this] {
    g_mutex_lock(&disp_lock);
    if (display)
      XCloseDisplay(display);
    display = nullptr;
    g_mutex_unlock(&disp_lock);
  });
  auto cleanup_pipelines = absl::MakeCleanup([this] {
    for (guint i = 0; i < num_instances; i++) {
      if (appCtx[i]) {
        if (appCtx[i]->return_value == -1)
          return_value = -1;
        destroy_pipeline(appCtx[i].get());
        g_mutex_lock(&disp_lock);
        if (windows[i])
          XDestroyWindow(display, windows[i]);
        windows[i] = 0;
        g_mutex_unlock(&disp_lock);
        appCtx[i].reset();
      }
    }
    g_mutex_lock(&disp_lock);
    if (display)
      XCloseDisplay(display);
    display = nullptr;
    g_mutex_unlock(&disp_lock);
    g_mutex_clear(&disp_lock);
  });
  // --- End cleanup objects ---

  // Set pipelines to PLAYING.
  for (guint i = 0; i < num_instances; i++) {
    absl::Status s = appCtx[i]->configurator().post_config_pipeline(appCtx[i]->pipeline, appCtx[i]->config);
    if (!s.ok()) {
      std::cerr << s << std::endl;
      g_print("\npipeline post-configuration failed.\n");
      return absl::InternalError("pipeline post-configuration failed");
    }
    if (gst_element_set_state(appCtx[i]->pipeline.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      g_print("\ncan't set pipeline to playing state.\n");
      return absl::InternalError("can't set pipeline to playing state");
    }
#if 1
    hm::save_dot_file(appCtx[i]->pipeline.pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "pipeline_running");
#endif
    if (appCtx[i]->config.pipeline_recreate_sec)
      g_timeout_add_seconds(
          appCtx[i]->config.pipeline_recreate_sec, recreate_pipeline_thread_func_static, appCtx[i].get());
  }

  print_runtime_commands();
  changemode(1);
  g_timeout_add(40, event_thread_func_static, nullptr);
  g_main_loop_run(main_loop);
  changemode(0);

  // After the main loop exits, update status if any instance flagged an error.
  if (return_value != 0)
    status = absl::InternalError("App run failed");
  else
    g_print("App run successful\n");

  return status;
}

//
// Main function: create a PipelineApplication instance and run it.
//
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
