/* clang-format off */
// X11 stuff must come first because it defines "Status"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#undef Status
/* clang-format on */

#include "PipelineApp.h"

#include <cuda_runtime_api.h>
#include <gst/gstbin.h>
#include <gst/video/videooverlay.h>
#include <signal.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

#include "hstream/src/apps/apps-common/deepstream_app_version.h"
#include "hstream/src/apps/apps-common/deepstream_common.h"
#include "hstream/src/libs/common/Status.h"

#include "absl/cleanup/cleanup.h"
#include "nvds_version.h"
#if 0
//------------------------------------------------------------------------------
// Debug category definition.
GST_DEBUG_CATEGORY(NVDS_APP);

//------------------------------------------------------------------------------
// CleanupStack implementation.
void CleanupStack::push(std::function<void()> cleanup) {
  cleanups_.push_back(std::move(cleanup));
}

CleanupStack::~CleanupStack() {
  // Invoke cleanup callbacks in reverse order.
  for (auto it = cleanups_.rbegin(); it != cleanups_.rend(); ++it) {
    (*it)();
  }
}

//------------------------------------------------------------------------------
// PipelineApplication static member definition.
PipelineApplication* PipelineApplication::instance_ = nullptr;

//------------------------------------------------------------------------------
// PipelineApplication constructor.
PipelineApplication::PipelineApplication()
    : cintr_(FALSE),
      main_loop_(nullptr),
      cfg_files_(nullptr),
      input_uris_(nullptr),
      game_id_(nullptr),
      print_version_(FALSE),
      show_bbox_text_(FALSE),
      print_dependencies_version_(FALSE),
      quit_(FALSE),
      dump_pipeline_dot_(FALSE),
      force_reconfigure_(FALSE),
      return_value_(0),
      // num_instances_(0),
      num_input_uris_(0),
      display_(nullptr),
      x_event_thread_(nullptr),
      rrow_(0),
      rcol_(0),
      rcfg_(0),
      rrowsel_(FALSE),
      selecting_(FALSE) {
  memset(fps_, 0, sizeof(fps_));
  memset(fps_avg_, 0, sizeof(fps_avg_));
  g_mutex_init(&fps_lock_);
  g_mutex_init(&disp_lock_);
  instance_ = this;
}

//------------------------------------------------------------------------------
// PipelineApplication destructor.
PipelineApplication::~PipelineApplication() {
  g_mutex_clear(&disp_lock_);
}

//------------------------------------------------------------------------------
// Callback and helper functions implementation.
// (The following implementations are largely as provided in the original source.)
//------------------------------------------------------------------------------

absl::Status PipelineApplication::initializeInstances(CleanupStack& /*cleanup_stack*/) {
  // Section 1: Create and initialize each HmApp instance.
  int i = -1;
  while (cfg_files_[++i]) {
    auto app_ctx = std::make_unique<HmApp>(game_id_ ? *game_id_ : "");
    // app_ctx = std::make_unique<HmApp>(game_id_ ? *game_id_ : "");
    app_ctx->person_class_id = -1;
    app_ctx->car_class_id = -1;
    app_ctx->index = i;
    app_ctx->active_source_index = -1;
    if (show_bbox_text_)
      app_ctx->show_bbox_text = TRUE;
    if (input_uris_ && input_uris_[i]) {
      app_ctx->config.multi_source_config[0].uri = g_strdup_printf("%s", input_uris_[i]);
      g_free(input_uris_[i]);
    }
    app_ctx->load_config();

    if (g_str_has_suffix(cfg_files_[i], ".yml") || g_str_has_suffix(cfg_files_[i], ".yaml")) {
      if (!app_ctx->underlay_config("pipeline", cfg_files_[i])) {
        NVGSTDS_ERR_MSG_V("Failed to merge in config file '%s'", cfg_files_[i]);
        app_ctx->return_value = -1;
        return absl::InternalError("Failed to merge in config file");
      }
      absl::Status configuration_status = app_ctx->complete_configuration(force_reconfigure_);
      if (configuration_status.code() == absl::StatusCode::kCancelled) {
        std::cerr << configuration_status;
        continue;
      }
      YAML::Node config = app_ctx->configurator().config();
      if (!config["pipeline"].IsDefined() || !parse_config_yaml(config["pipeline"], &app_ctx->config, cfg_files_[i])) {
        NVGSTDS_ERR_MSG_V("Failed to parse config file '%s'", cfg_files_[i]);
        app_ctx->return_value = -1;
        return absl::InternalError("Failed to parse config file");
      }
    } else if (g_str_has_suffix(cfg_files_[i], ".txt")) {
      if (!parse_config_file(&app_ctx->config, cfg_files_[i])) {
        NVGSTDS_ERR_MSG_V("Failed to parse config file '%s'", cfg_files_[i]);
        app_ctx->return_value = -1;
        return absl::InternalError("Failed to parse config file");
      }
    }
    stage_app_contexts_[app_ctx->config.stage].emplace_back(std::move(app_ctx));
  }
  if (!stage_app_contexts_.empty()) {
    current_stage_ = stage_app_contexts_.begin()->first;
  }
  return absl::OkStatus();
}

absl::Status PipelineApplication::createPipelines(
    std::vector<std::shared_ptr<HmApp>>& app_contexts,
    CleanupStack& /*cleanup_stack*/) const {
  // Section 2: Create pipelines for each instance.
  for (guint i = 0; i < app_contexts.size(); i++) {
    if (!create_pipeline(app_contexts[i].get(), nullptr, all_bbox_generated, perf_cb_static, overlay_graphics_static)) {
      NVGSTDS_ERR_MSG_V("Failed to create pipeline");
      return absl::InternalError("Failed to create pipeline");
    }
    if (dump_pipeline_dot_) {
      std::string s = "pipeline";
      if (i) {
        s += '_';
        s += std::to_string(i);
      }
      gst_debug_bin_to_dot_file_with_ts(
          GST_BIN(app_contexts[i]->pipeline.pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "/mnt/data/src/hstream/pipeline.dot");
    }
  }
  return absl::OkStatus();
}

absl::Status PipelineApplication::createMainLoop(
    std::vector<std::shared_ptr<HmApp>>& app_contexts,
    std::map<int, Window>& windows,
    CleanupStack& cleanup_stack) {
  // Section 3: Create main loop and initialize display/windows.
  main_loop_ = g_main_loop_new(nullptr, FALSE);
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
  cleanup_stack.push([this] {
    g_mutex_lock(&disp_lock_);
    if (display_)
      XCloseDisplay(display_);
    display_ = nullptr;
    g_mutex_unlock(&disp_lock_);
  });

  for (guint i = 0; i < app_contexts.size(); i++) {
#if defined(__aarch64__)
    if (gst_element_set_state(app_contexts[i]->pipeline.pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
      NVGSTDS_ERR_MSG_V("Failed to set pipeline to PAUSED");
      return absl::InternalError("Failed to set pipeline to PAUSED");
    }
#endif
    for (guint j = 0; j < app_contexts[i]->config.num_sink_sub_bins; j++) {
      if (!GST_IS_VIDEO_OVERLAY(app_contexts[i]->pipeline.instance_bins[0].sink_bin.sub_bins[j].sink))
        continue;

      guint width = 0, height = 0;
      XSizeHints hints = {0};
      if (app_contexts[i]->config.sink_bin_sub_bin_config[j].render_config.width)
        width = app_contexts[i]->config.sink_bin_sub_bin_config[j].render_config.width;
      else
        width = app_contexts[i]->config.tiled_display_config.width;
      if (app_contexts[i]->config.sink_bin_sub_bin_config[j].render_config.height)
        height = app_contexts[i]->config.sink_bin_sub_bin_config[j].render_config.height;
      else
        height = app_contexts[i]->config.tiled_display_config.height;
      width = (width) ? width : DEFAULT_X_WINDOW_WIDTH;
      height = (height) ? height : DEFAULT_X_WINDOW_HEIGHT;

      hints.flags = PPosition | PSize;
      hints.x = app_contexts[i]->config.sink_bin_sub_bin_config[j].render_config.offset_x;
      hints.y = app_contexts[i]->config.sink_bin_sub_bin_config[j].render_config.offset_y;
      hints.width = width;
      hints.height = height;

      assert(!windows.count(i));
      windows[i] = XCreateSimpleWindow(
          display_,
          RootWindow(display_, DefaultScreen(display_)),
          hints.x,
          hints.y,
          width,
          height,
          2,
          0x00000000,
          0x00000000);

      XSetNormalHints(display_, windows[i], &hints);

      gchar* title = (app_contexts.size() > 1) ? g_strdup_printf(APP_TITLE "-%d", i) : g_strdup(APP_TITLE);
      XTextProperty xproperty;
      if (XStringListToTextProperty((char**)&title, 1, &xproperty) != 0) {
        XSetWMName(display_, windows[i], &xproperty);
        XFree(xproperty.value);
      }

      XSetWindowAttributes attr = {0};
      if ((app_contexts[i]->config.tiled_display_config.enable &&
           app_contexts[i]->config.tiled_display_config.rows * app_contexts[i]->config.tiled_display_config.columns ==
               1) ||
          (app_contexts[i]->config.tiled_display_config.enable == 0))
        attr.event_mask = KeyPress;
      else if (app_contexts[i]->config.tiled_display_config.enable)
        attr.event_mask = ButtonPress | KeyRelease;
      XChangeWindowAttributes(display_, windows[i], CWEventMask, &attr);

      Atom wmDeleteMessage = XInternAtom(display_, "WM_DELETE_WINDOW", False);
      if (wmDeleteMessage != None)
        XSetWMProtocols(display_, windows[i], &wmDeleteMessage, 1);

      XMapRaised(display_, windows[i]);
      XSync(display_, 1);
      gst_video_overlay_set_window_handle(
          GST_VIDEO_OVERLAY(app_contexts[i]->pipeline.instance_bins[0].sink_bin.sub_bins[j].sink), (gulong)windows[i]);
      gst_video_overlay_expose(GST_VIDEO_OVERLAY(app_contexts[i]->pipeline.instance_bins[0].sink_bin.sub_bins[j].sink));

      if (!x_event_thread_)
        x_event_thread_ = g_thread_new("nvds-window-event-thread", nvds_x_event_thread_static, nullptr);
    }
#if !defined(__aarch64__)
    int current_device = -1;
    cudaGetDevice(&current_device);
    struct cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, current_device);
    if (!prop.integrated) {
      if (gst_element_set_state(app_contexts[i]->pipeline.pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
        NVGSTDS_ERR_MSG_V("Failed to set pipeline to PAUSED");
        return absl::InternalError("Failed to set pipeline to PAUSED");
      }
    }
#endif
  }
  cleanup_stack.push([this, contexts = app_contexts, windows = windows]() mutable -> void {
    for (guint i = 0; i < contexts.size(); i++) {
      if (contexts[i]) {
        if (contexts[i]->return_value == -1)
          return_value_ = -1;
        destroy_pipeline(contexts[i].get());
        g_mutex_lock(&disp_lock_);
        if (windows[i]) {
          XDestroyWindow(display_, windows[i]);
        }
        windows[i] = 0;
        g_mutex_unlock(&disp_lock_);
        contexts[i].reset();
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

absl::Status PipelineApplication::playPipelines(
    std::vector<std::shared_ptr<HmApp>>& app_contexts,
    CleanupStack& /*cleanup_stack*/) const {
  absl::Status status;
  for (guint i = 0; i < app_contexts.size(); i++) {
    status = app_contexts[i]->configurator().post_config_pipeline(app_contexts[i]->pipeline, app_contexts[i]->config);
    if (!status.ok()) {
      std::cerr << status << std::endl;
      g_print("\npipeline post-configuration failed.\n");
      return absl::InternalError("pipeline post-configuration failed");
    }
    if (gst_element_set_state(app_contexts[i]->pipeline.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      g_print("\ncan't set pipeline to playing state.\n");
      return absl::InternalError("can't set pipeline to playing state");
    }
#if 1
    hm::save_dot_file(app_contexts[i]->pipeline.pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "pipeline_running");
#endif
    if (app_contexts[i]->config.pipeline_recreate_sec)
      g_timeout_add_seconds(
          app_contexts[i]->config.pipeline_recreate_sec, recreate_pipeline_thread_func_static, app_contexts[i].get());
  }

  print_runtime_commands();
  changemode(1);
  g_timeout_add(40, event_thread_func_static, nullptr);
  g_main_loop_run(main_loop_);
  changemode(0);

  if (return_value_ != 0)
    status = absl::InternalError("App run failed");
  else
    g_print("App run successful\n");

  return status;
}

absl::Status PipelineApplication::waitForPipelinesStopped(std::vector<std::shared_ptr<HmApp>>& app_contexts) const {
  for (auto app_ctx : app_contexts) {
    if (!app_ctx) {
      continue;
    }
    if (!app_ctx->pipeline.pipeline) {
      continue;
    }
    hm::waitForPipelineStop(app_ctx->pipeline.pipeline);
  }
  return absl::OkStatus();
}

//------------------------------------------------------------------------------
// Main run function.
//------------------------------------------------------------------------------
absl::Status PipelineApplication::run(int argc, char* argv[]) {
  absl::Status status = absl::OkStatus();
  GError* error = nullptr;

  CleanupStack global_cleanup_stack;

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

  if (input_uris_) {
    num_input_uris_ = g_strv_length(input_uris_);
  }
  if (!cfg_files_ || g_strv_length(cfg_files_) == 0) {
    NVGSTDS_ERR_MSG_V("Specify config file with -c option");
    return absl::InternalError("Specify config file with -c option");
  }

  HM_RETURN_IF_ERROR(initializeInstances(global_cleanup_stack));

  for (auto stage_item : stage_app_contexts_) {
    current_stage_ = stage_item.first;
    auto& app_contexts = stage_app_contexts_.at(current_stage_);
    {
      CleanupStack stage_cleanup_stack;
      HM_RETURN_IF_ERROR(createPipelines(app_contexts, stage_cleanup_stack));
      HM_RETURN_IF_ERROR(createMainLoop(app_contexts, stage_windows_[current_stage_], stage_cleanup_stack));
      HM_RETURN_IF_ERROR(playPipelines(app_contexts, stage_cleanup_stack));
    }
    HM_RETURN_IF_ERROR(waitForPipelinesStopped(app_contexts));
  }
  return absl::OkStatus();
}

//------------------------------------------------------------------------------
// Other static and member function implementations.
//
// (Implementations for functions such as all_bbox_generated, perf_cb, event handling,
// changemode, overlay_graphics, pipeline recreation, etc. are taken from the original source.)
//------------------------------------------------------------------------------

void PipelineApplication::all_bbox_generated(AppCtx* app_ctx, GstBuffer* buf, NvDsBatchMeta* batch_meta, guint index) {
  guint num_male = 0;
  guint num_female = 0;
  guint num_objects[128];
  memset(num_objects, 0, sizeof(num_objects));

  for (NvDsMetaList* l_frame = batch_meta->frame_meta_list; l_frame != nullptr; l_frame = l_frame->next) {
    NvDsFrameMeta* frame_meta = reinterpret_cast<NvDsFrameMeta*>(l_frame->data);
    for (NvDsMetaList* l_obj = frame_meta->obj_meta_list; l_obj != nullptr; l_obj = l_obj->next) {
      NvDsObjectMeta* obj = reinterpret_cast<NvDsObjectMeta*>(l_obj->data);
      if (obj->unique_component_id == static_cast<gint>(app_ctx->config.primary_gie_config.unique_id)) {
        if (obj->class_id >= 0 && obj->class_id < 128)
          num_objects[obj->class_id]++;
        if (app_ctx->person_class_id > -1 && obj->class_id == app_ctx->person_class_id) {
          if (strstr(obj->text_params.display_text, "Man")) {
            str_replace(obj->text_params.display_text, "Man", "");
            str_replace(obj->text_params.display_text, "Person", "Man");
            num_male++;
            (void)num_male;
          } else if (strstr(obj->text_params.display_text, "Woman")) {
            str_replace(obj->text_params.display_text, "Woman", "");
            str_replace(obj->text_params.display_text, "Person", "Woman");
            num_female++;
            (void)num_female;
          }
        }
      }
    }
  }
}

void PipelineApplication::_intr_handler(int signum) {
  if (instance_)
    instance_->handle_intr(signum);
}

void PipelineApplication::handle_intr(int signum) {
  NVGSTDS_ERR_MSG_V("User Interrupted..\n");
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_DFL;
  sigaction(SIGINT, &action, nullptr);
  cintr_ = TRUE;
}

void PipelineApplication::_intr_setup() {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = _intr_handler;
  sigaction(SIGINT, &action, nullptr);
}

void PipelineApplication::perf_cb_static(gpointer context, NvDsAppPerfStruct* str) {
  if (instance_)
    instance_->perf_cb(context, str);
}

void PipelineApplication::perf_cb(gpointer context, NvDsAppPerfStruct* str) {
  static guint header_print_cnt = 0;
  guint i;
  AppCtx* app_ctx = reinterpret_cast<AppCtx*>(context);
  guint numf = str->num_instances;

  g_mutex_lock(&fps_lock_);
  for (i = 0; i < numf; i++) {
    fps_[i] = str->fps[i];
    fps_avg_[i] = str->fps_avg[i];
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
  if (stage_app_contexts_.at(current_stage_).size() > 1)
    g_print("PERF(%d): ", app_ctx->index);
  else
    g_print("**PERF:  ");
  for (i = 0; i < numf; i++) {
    g_print("%.2f (%.2f)\t", fps_[i], fps_avg_[i]);
  }
  g_print("\n");
  g_mutex_unlock(&fps_lock_);
}

gboolean PipelineApplication::check_for_interrupt_static(gpointer data) {
  if (instance_)
    return instance_->check_for_interrupt();
  return TRUE;
}

gboolean PipelineApplication::check_for_interrupt() {
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

gboolean PipelineApplication::kbhit() {
  struct timeval tv;
  fd_set rdfs;
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  FD_ZERO(&rdfs);
  FD_SET(STDIN_FILENO, &rdfs);
  select(STDIN_FILENO + 1, &rdfs, nullptr, nullptr, &tv);
  return FD_ISSET(STDIN_FILENO, &rdfs);
}

void PipelineApplication::changemode(int dir) {
  static struct termios oldt, newt;
  if (dir == 1) {
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
  } else {
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
  }
}

void PipelineApplication::print_runtime_commands() const {
  g_print(
      "\nRuntime commands:\n"
      "\th: Print this help\n"
      "\tq: Quit\n\n"
      "\tp: Pause\n"
      "\tr: Resume\n\n");
  if (!stage_app_contexts_.empty() && !stage_app_contexts_.at(current_stage_).empty() &&
      stage_app_contexts_.at(current_stage_)[0] &&
      stage_app_contexts_.at(current_stage_)[0]->config.tiled_display_config.enable) {
    g_print(
        "NOTE: To expand a source in the 2D tiled display and view object details,\n"
        "      left-click on the source.\n"
        "      To go back to the tiled display, right-click anywhere on the window.\n\n");
  }
}

gboolean PipelineApplication::event_thread_func_static(gpointer arg) {
  if (instance_)
    return instance_->event_thread_func();
  return TRUE;
}

gboolean PipelineApplication::event_thread_func() {
  guint i;
  gboolean ret = TRUE;

  auto& app_ctx = stage_app_contexts_.at(current_stage_);
  for (i = 0; i < app_ctx.size(); i++) {
    if (app_ctx[i] && !app_ctx[i]->quit)
      break;
  }
  if (i == app_ctx.size()) {
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
  GstElement* tiler = (app_ctx[rcfg_]) ? app_ctx[rcfg_]->pipeline.tiled_display_bin.tiler : nullptr;
  if (app_ctx[rcfg_] && app_ctx[rcfg_]->config.tiled_display_config.enable && tiler) {
    g_object_get(G_OBJECT(tiler), "show-source", &source_id, nullptr);
    if (selecting_) {
      if (!rrowsel_) {
        if (c >= '0' && c <= '9') {
          rrow_ = c - '0';
          if (rrow_ < app_ctx[rcfg_]->config.tiled_display_config.rows) {
            g_print("--selecting source  row %d--\n", rrow_);
            rrowsel_ = TRUE;
          } else {
            g_print("--selected source  row %d out of bound, reenter\n", rrow_);
          }
        }
      } else {
        if (c >= '0' && c <= '9') {
          unsigned int tile_num_columns = app_ctx[rcfg_]->config.tiled_display_config.columns;
          rcol_ = c - '0';
          if (rcol_ < tile_num_columns) {
            selecting_ = FALSE;
            rrowsel_ = FALSE;
            source_id = tile_num_columns * rrow_ + rcol_;
            g_print("--selecting source  col %d sou=%d--\n", rcol_, source_id);
            if (source_id >= (gint)app_ctx[rcfg_]->config.num_source_sub_bins)
              source_id = -1;
            else {
              app_ctx[rcfg_]->show_bbox_text = TRUE;
              app_ctx[rcfg_]->active_source_index = source_id;
              g_object_set(G_OBJECT(tiler), "show-source", source_id, nullptr);
            }
          } else {
            g_print("--selected source  col %d out of bound, reenter\n", rcol_);
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
      for (i = 0; i < app_ctx.size(); i++)
        if (app_ctx[i])
          pause_pipeline(app_ctx[i].get());
      break;
    case 'r':
      for (i = 0; i < app_ctx.size(); i++)
        if (app_ctx[i])
          resume_pipeline(app_ctx[i].get());
      break;
    case 'q':
      quit_ = TRUE;
      if (main_loop_)
        g_main_loop_quit(main_loop_);
      ret = FALSE;
      break;
    case 'c':
      if (app_ctx[rcfg_] && app_ctx[rcfg_]->config.tiled_display_config.enable && selecting_ == FALSE &&
          source_id == -1) {
        g_print("--selecting config file --\n");
        c = fgetc(stdin);
        if (c >= '0' && c <= '9') {
          rcfg_ = c - '0';
          if (rcfg_ < app_ctx.size())
            g_print("--selecting config  %d--\n", rcfg_);
          else {
            g_print("--selected config file %d out of bound, reenter\n", rcfg_);
            rcfg_ = 0;
          }
        }
      }
      break;
    case 'z':
      if (app_ctx[rcfg_] && app_ctx[rcfg_]->config.tiled_display_config.enable && source_id == -1 &&
          selecting_ == FALSE) {
        g_print("--selecting source --\n");
        selecting_ = TRUE;
      } else {
        if (!show_bbox_text_ && app_ctx[rcfg_])
          app_ctx[rcfg_]->show_bbox_text = FALSE;
        if (tiler)
          g_object_set(G_OBJECT(tiler), "show-source", -1, nullptr);
        if (app_ctx[rcfg_])
          app_ctx[rcfg_]->active_source_index = -1;
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

int PipelineApplication::get_source_id_from_coordinates(float x_rel, float y_rel, AppCtx* app_ctx) {
  int tile_num_rows = app_ctx->config.tiled_display_config.rows;
  int tile_num_columns = app_ctx->config.tiled_display_config.columns;
  int source_id = static_cast<int>(x_rel * tile_num_columns);
  source_id += (static_cast<int>(y_rel * tile_num_rows)) * tile_num_columns;
  if (source_id >= (gint)app_ctx->config.num_source_sub_bins)
    source_id = -1;
  return source_id;
}

gpointer PipelineApplication::nvds_x_event_thread_static(gpointer data) {
  if (instance_)
    return instance_->nvds_x_event_thread();
  return nullptr;
}

gpointer PipelineApplication::nvds_x_event_thread() {
  g_mutex_lock(&disp_lock_);
  while (display_) {
    XEvent e;
    guint index;
    memset(&e, 0, sizeof(XEvent));
    while (XPending(display_)) {
      XNextEvent(display_, &e);
      auto& app_ctx = stage_app_contexts_.at(current_stage_);
      switch (e.type) {
        case ButtonPress: {
          XWindowAttributes win_attr;
          XButtonEvent ev = e.xbutton;
          gint source_id = -1;
          GstElement* tiler = nullptr;
          memset(&win_attr, 0, sizeof(XWindowAttributes));
          XGetWindowAttributes(display_, ev.window, &win_attr);
          for (index = 0; index < app_ctx.size(); index++)
            if (ev.window == stage_windows_.at(current_stage_)[index])
              break;
          tiler =
              (index < app_ctx.size() && app_ctx[index]) ? app_ctx[index]->pipeline.tiled_display_bin.tiler : nullptr;
          if (ev.button == Button1 && source_id == -1 && (index < app_ctx.size())) {
            if (app_ctx[index])
              source_id = get_source_id_from_coordinates(
                  ev.x * 1.0f / win_attr.width, ev.y * 1.0f / win_attr.height, app_ctx[index].get());
            else
              source_id = -1;
            if (source_id > -1 && tiler) {
              g_object_set(G_OBJECT(tiler), "show-source", source_id, nullptr);
              app_ctx[index]->active_source_index = source_id;
              app_ctx[index]->show_bbox_text = TRUE;
            }
          } else if (ev.button == Button3) {
            if (tiler)
              g_object_set(G_OBJECT(tiler), "show-source", -1, nullptr);
            if (app_ctx[index])
              app_ctx[index]->active_source_index = -1;
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
            for (i = 0; i < app_ctx.size(); i++)
              if (app_ctx[i])
                pause_pipeline(app_ctx[i].get());
            break;
          }
          if (e.xkey.keycode == r) {
            for (i = 0; i < app_ctx.size(); i++)
              if (app_ctx[i])
                resume_pipeline(app_ctx[i].get());
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
          for (index = 0; index < app_ctx.size(); index++)
            if (e.xclient.window == stage_windows_.at(current_stage_)[index])
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

gboolean PipelineApplication::overlay_graphics_static(
    AppCtx* app_ctx,
    GstBuffer* buf,
    NvDsBatchMeta* batch_meta,
    guint index) {
  return instance_ ? instance_->overlay_graphics(app_ctx, buf, batch_meta, index) : TRUE;
}

gboolean PipelineApplication::overlay_graphics(
    AppCtx* app_ctx,
    GstBuffer* buf,
    NvDsBatchMeta* batch_meta,
    guint index) {
  int src_index = app_ctx->active_source_index;
  if (src_index == -1)
    return TRUE;
  NvDsFrameLatencyInfo* latency_info = nullptr;
  NvDsDisplayMeta* display_meta = nvds_acquire_display_meta_from_pool(batch_meta);

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
  nvds_add_display_meta_to_frame(nvds_get_nth_frame_meta(batch_meta->frame_meta_list, 0), display_meta);
  return TRUE;
}

gboolean PipelineApplication::recreate_pipeline_thread_func_static(gpointer arg) {
  return instance_ ? instance_->recreate_pipeline_thread_func(arg) : FALSE;
}

gboolean PipelineApplication::recreate_pipeline_thread_func(gpointer arg) {
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
  if (gst_element_set_state(app_ctx_ptr->pipeline.pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
    NVGSTDS_ERR_MSG_V("Failed to set pipeline to PAUSED");
    return absl::InternalError("Failed to set pipeline to PAUSED").raw_code();
  }
  for (i = 0; i < app_ctx_ptr->config.num_sink_sub_bins; i++) {
    if (!GST_IS_VIDEO_OVERLAY(app_ctx_ptr->pipeline.instance_bins[0].sink_bin.sub_bins[i].sink))
      continue;
    gst_video_overlay_set_window_handle(
        GST_VIDEO_OVERLAY(app_ctx_ptr->pipeline.instance_bins[0].sink_bin.sub_bins[i].sink),
        (gulong)stage_windows_.at(current_stage_)[app_ctx_ptr->index]);
    gst_video_overlay_expose(GST_VIDEO_OVERLAY(app_ctx_ptr->pipeline.instance_bins[0].sink_bin.sub_bins[i].sink));
  }
  if (gst_element_set_state(app_ctx_ptr->pipeline.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_print("\ncan't set pipeline to playing state.\n");
    return absl::InternalError("can't set pipeline to playing state").raw_code();
  }
  return ret;
}

//------------------------------------------------------------------------------
// Main function.
//------------------------------------------------------------------------------
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
#endif
