/* clang-format off */
// X11 stuff must come first becaus eit defined "Status"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#undef Status
/* clang-format on */

#undef Status
/* clang-format on */

#include <cuda_runtime_api.h>
#include <getopt.h>
#include <gst/gst.h>
#include <gst/gstbin.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Include DeepStream headers.
#include "deepstream_app.h"
#include "hstream/src/apps/apps-common/deepstream_app_version.h"
#include "hstream/src/apps/apps-common/deepstream_common.h"
#include "hstream/src/apps/apps-common/deepstream_config_file_parser.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/pipeline_utils.h"

#include "nvds_version.h"

// Constants.
constexpr int MAX_INSTANCES = 128;
constexpr char APP_TITLE[] = "DeepStream";
constexpr int DEFAULT_X_WINDOW_WIDTH = 1920;
constexpr int DEFAULT_X_WINDOW_HEIGHT = 1080;

static constexpr const char* kConfigureStitchingConfigFileName = "ds_hockey_configure_stitching.yaml";

GST_DEBUG_CATEGORY(NVDS_APP);

//------------------------------------------------------------------------------
// DeepStreamApplication class
//------------------------------------------------------------------------------
class DeepStreamApplication {
 public:
  DeepStreamApplication(int argc, char* argv[]);
  ~DeepStreamApplication();
  absl::Status run();

  // Static signal handler.
  static void signalHandler(int signum);

 private:
  // Command-line parsing using getopt.
  void parseCommandLine(int argc, char* argv[]);

  // Pipeline and window initialization.
  bool initPipelines();
  bool initXWindows();

  // Event loop and thread functions.
  void startEventLoop();
  void keyboardEventThread();
  void xEventThread();

  // Terminal mode control.
  void changeTerminalMode(bool enable);

  // Signal interrupt handling.
  void handleInterrupt(int signum);

  // Helper for printing runtime commands.
  void printRuntimeCommands();

  // Cleanup function.
  void cleanup();

  // ----- Callback stubs -----
  // These static callbacks will be registered with the DeepStream pipeline.
  // They delegate to member functions or free functions as needed.
  static void allBBoxGeneratedCallback(AppCtx* appCtx, GstBuffer* buf, NvDsBatchMeta* batch_meta, guint index) {
    // (Implementation similar to the C version)
  }
  static void perfCallback(gpointer context, NvDsAppPerfStruct* str) {
    // (Implementation similar to the C version)
  }
  static gboolean overlayGraphicsCallback(AppCtx* appCtx, GstBuffer* buf, NvDsBatchMeta* batch_meta, guint index) {
    // (Implementation similar to the C version)
    return TRUE;
  }
  static gboolean recreatePipelineThreadCallback(gpointer arg) {
    // (Implementation similar to the C version)
    return TRUE;
  }
  // ----------------------------

  // Members that were formerly globals:
  std::vector<std::unique_ptr<HmApp>> apps_; // Each pipeline instance.
  GMainLoop* main_loop_ = nullptr;
  std::vector<std::string> cfg_files_;
  std::vector<std::string> input_uris_;
  std::string game_id_;

  bool print_version_ = false;
  bool show_bbox_text_ = false;
  bool print_dependencies_version_ = false;
  bool dump_pipeline_dot_ = false;

  bool quit_ = false;
  int return_value_ = 0;
  unsigned num_instances_ = 0;
  unsigned num_input_uris_ = 0;

  std::mutex fps_mutex_;
  std::vector<double> fps_;
  std::vector<double> fps_avg_;

  // X11-related members.
  Display* display_ = nullptr;
  std::vector<Window> windows_;
  std::thread x_event_thread_;
  std::mutex disp_mutex_;

  // Variables used for tiled display selection.
  unsigned rrow_ = 0, rcol_ = 0, rcfg_ = 0;
  bool rrowsel_ = false;
  bool selecting_ = false;

  // Terminal mode backup.
  struct termios old_termios_;
  bool termios_saved_ = false;

  // Static pointer to the current instance (for signal handling).
  static DeepStreamApplication* instance_;
};

// Initialize static instance pointer.
DeepStreamApplication* DeepStreamApplication::instance_ = nullptr;

//------------------------------------------------------------------------------
// Constructor: parses options and sets up initial state.
//------------------------------------------------------------------------------
DeepStreamApplication::DeepStreamApplication(int argc, char* argv[]) {
  instance_ = this; // For signal handling.
  parseCommandLine(argc, argv);
}

//------------------------------------------------------------------------------
// Destructor: cleans up resources.
//------------------------------------------------------------------------------
DeepStreamApplication::~DeepStreamApplication() {
  cleanup();
  instance_ = nullptr;
}

//------------------------------------------------------------------------------
// parseCommandLine: use getopt (or getopt_long) to fill in member variables.
//------------------------------------------------------------------------------
void DeepStreamApplication::parseCommandLine(int argc, char* argv[]) {
  const char* short_opts = "vtdc:g:i:";
  const option long_opts[] = {{"version-all", no_argument, nullptr, 0}, {nullptr, 0, nullptr, 0}};

  int opt;
  int long_index = 0;
  while ((opt = getopt_long(argc, argv, short_opts, long_opts, &long_index)) != -1) {
    switch (opt) {
      case 'v':
        print_version_ = true;
        break;
      case 't':
        show_bbox_text_ = true;
        break;
      case 'd':
        dump_pipeline_dot_ = true;
        break;
      case 'c':
        cfg_files_.push_back(optarg);
        break;
      case 'g':
        game_id_ = optarg;
        break;
      case 'i':
        input_uris_.push_back(optarg);
        break;
      case 0: // Long options without a short option.
        if (std::string(long_opts[long_index].name) == "version-all") {
          print_dependencies_version_ = true;
        }
        break;
      default:
        std::cerr << "Unknown option" << std::endl;
        break;
    }
  }

  if (cfg_files_.empty()) {
    std::cerr << "Specify config file with -c option" << std::endl;
    return_value_ = -1;
  }
  num_instances_ = cfg_files_.size();
  num_input_uris_ = input_uris_.size();
}

//------------------------------------------------------------------------------
// run: main function that initializes pipelines, windows, event loops, and runs the main loop.
//------------------------------------------------------------------------------
absl::Status DeepStreamApplication::run() {
  absl::Status status;

  // Initialize GStreamer.
  gst_init(nullptr, nullptr);

  // Print version info if requested.
  if (print_version_ || print_dependencies_version_) {
    std::cout << "deepstream-app version " << NVDS_APP_VERSION_MAJOR << "." << NVDS_APP_VERSION_MINOR << "."
              << NVDS_APP_VERSION_MICRO << "\n";
    nvds_version_print();
    if (print_dependencies_version_) {
      nvds_dependencies_version_print();
    }
    return status;
  }

  // Initialize CUDA device.
  int current_device = -1;
  cudaGetDevice(&current_device);
  struct cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, current_device);

  // Create a HmApp instance for each config file.
  if (!initPipelines()) {
    return absl::InternalError("Failed to initialize pipelines");
  }

  // Create the main loop.
  main_loop_ = g_main_loop_new(nullptr, FALSE);

  // Set up SIGINT handler.
  struct sigaction action;
  std::memset(&action, 0, sizeof(action));
  action.sa_handler = DeepStreamApplication::signalHandler;
  sigaction(SIGINT, &action, nullptr);

  // Initialize X11 display and windows.
  if (!initXWindows()) {
    return absl::InternalError("Failed to initialize X Windows");
  }

  // Set pipelines to PLAYING (error checking omitted for brevity).
  for (unsigned i = 0; i < num_instances_; i++) {
    if (!create_pipeline(apps_[i].get(), nullptr, allBBoxGeneratedCallback, perfCallback, overlayGraphicsCallback)) {
      std::cerr << "Failed to create pipeline" << std::endl;
      return_value_ = -1;
      return absl::InternalError("Pipeline creation error");
    }
    gst_element_set_state(apps_[i]->pipeline.pipeline, GST_STATE_PLAYING);
  }

  // Optionally dump dot files.
  if (dump_pipeline_dot_) {
    std::string s = "pipeline";
    if (num_instances_ > 1) {
      s += '_' + std::to_string(0);
    }
    gst_debug_bin_to_dot_file_with_ts(
        GST_BIN(apps_[0]->pipeline.pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "/mnt/data/src/hstream/pipeline.dot");
  }

  // Print runtime commands.
  printRuntimeCommands();

  // Change terminal mode to non-canonical.
  changeTerminalMode(true);

  // Add periodic timeout to check for interrupt.
  g_timeout_add(
      400,
      [](gpointer) -> gboolean {
        if (instance_ && instance_->quit_) {
          g_main_loop_quit(instance_->main_loop_);
          return FALSE;
        }
        return TRUE;
      },
      nullptr);

  // Start a thread to process X11 events.
  x_event_thread_ = std::thread(&DeepStreamApplication::xEventThread, this);

  // Start keyboard event handling (in the main thread or a separate thread).
  std::thread keyboard_thread(&DeepStreamApplication::keyboardEventThread, this);

  // Run main loop.
  g_main_loop_run(main_loop_);

  // Restore terminal mode.
  changeTerminalMode(false);

  // Join threads.
  if (keyboard_thread.joinable())
    keyboard_thread.join();
  if (x_event_thread_.joinable())
    x_event_thread_.join();

  // Cleanup pipelines and windows.
  cleanup();

  if (return_value_ == 0)
    std::cout << "App run successful" << std::endl;
  else
    status.Update(absl::InternalError("App run failed"));

  gst_deinit();
  return status;
}

//------------------------------------------------------------------------------
// initPipelines: create and configure each HmApp instance.
//------------------------------------------------------------------------------
bool DeepStreamApplication::initPipelines() {
  for (unsigned i = 0; i < num_instances_; i++) {
    auto app = std::make_unique<HmApp>(game_id_);
    app->person_class_id = -1;
    app->car_class_id = -1;
    app->index = i;
    app->active_source_index = -1;
    if (show_bbox_text_)
      app->show_bbox_text = TRUE;

    // If an input URI is provided for this instance, update configuration.
    if (i < input_uris_.size()) {
      app->config.multi_source_config[0].uri = g_strdup(input_uris_[i].c_str());
    }
    app->load_config();

    // Merge in additional configuration from file.
    if (g_str_has_suffix(cfg_files_[i].c_str(), ".yml") || g_str_has_suffix(cfg_files_[i].c_str(), ".yaml")) {
      if (!app->underlay_config("pipeline", cfg_files_[i].c_str())) {
        std::cerr << "Failed to merge config file: " << cfg_files_[i] << std::endl;
        app->return_value = -1;
        return false;
      }
      auto status = app->complete_configuration(/*force=*/false);
      if (!status.ok()) {
        std::cerr << status << std::endl;
        return false;
      }
      YAML::Node config = app->configurator().config();
      std::cout << config << std::endl;
      if (!config["pipeline"].IsDefined() ||
          !parse_config_yaml(config["pipeline"], &app->config, cfg_files_[i].c_str())) {
        std::cerr << "Failed to parse config file: " << cfg_files_[i] << std::endl;
        app->return_value = -1;
        return false;
      }
    } else if (g_str_has_suffix(cfg_files_[i].c_str(), ".txt")) {
      if (!parse_config_file(&app->config, cfg_files_[i].c_str())) {
        std::cerr << "Failed to parse config file: " << cfg_files_[i] << std::endl;
        app->return_value = -1;
        return false;
      }
    }
    apps_.push_back(std::move(app));
  }
  return true;
}

//------------------------------------------------------------------------------
// initXWindows: open the X display and create windows for each pipeline instance.
//------------------------------------------------------------------------------
bool DeepStreamApplication::initXWindows() {
  display_ = XOpenDisplay(nullptr);
  if (!display_) {
    std::cerr << "Could not open X Display" << std::endl;
    return false;
  }
  windows_.resize(num_instances_, 0);

  for (unsigned i = 0; i < num_instances_; i++) {
    // Determine window dimensions.
    guint width = apps_[i]->config.sink_bin_sub_bin_config[0].render_config.width;
    guint height = apps_[i]->config.sink_bin_sub_bin_config[0].render_config.height;
    if (!width)
      width = apps_[i]->config.tiled_display_config.width;
    if (!height)
      height = apps_[i]->config.tiled_display_config.height;
    if (!width)
      width = DEFAULT_X_WINDOW_WIDTH;
    if (!height)
      height = DEFAULT_X_WINDOW_HEIGHT;

    // Create X window.
    XSetWindowAttributes attr = {};
    attr.event_mask = KeyPress | ButtonPress | KeyRelease;
    Window win = XCreateSimpleWindow(display_, DefaultRootWindow(display_), 0, 0, width, height, 2, 0, 0);
    // Set window title.
    std::string title = (num_instances_ > 1) ? (std::string(APP_TITLE) + "-" + std::to_string(i)) : APP_TITLE;
    XStoreName(display_, win, title.c_str());
    // Enable window deletion.
    Atom wmDeleteMessage = XInternAtom(display_, "WM_DELETE_WINDOW", False);
    if (wmDeleteMessage != None)
      XSetWMProtocols(display_, win, &wmDeleteMessage, 1);
    // Map the window.
    XMapWindow(display_, win);
    XSync(display_, False);

    windows_[i] = win;
    // Link the window to the pipeline’s video overlay.
    gst_video_overlay_set_window_handle(
        GST_VIDEO_OVERLAY(apps_[i]->pipeline.instance_bins[0].sink_bin.sub_bins[0].sink), (gulong)win);
    gst_video_overlay_expose(GST_VIDEO_OVERLAY(apps_[i]->pipeline.instance_bins[0].sink_bin.sub_bins[0].sink));
  }
  return true;
}

//------------------------------------------------------------------------------
// startEventLoop: runs the main loop (already invoked in run()).
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// keyboardEventThread: continuously polls for keyboard events (non-canonical mode).
//------------------------------------------------------------------------------
void DeepStreamApplication::keyboardEventThread() {
  while (!quit_) {
    // Check if a key is pressed (nonblocking).
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    timeval timeout = {0, 0};
    int rv = select(STDIN_FILENO + 1, &set, nullptr, nullptr, &timeout);
    if (rv > 0 && FD_ISSET(STDIN_FILENO, &set)) {
      int c = fgetc(stdin);
      std::cout << "\n";
      // Process key commands.
      switch (c) {
        case 'h':
          printRuntimeCommands();
          break;
        case 'p':
          for (auto& app : apps_) {
            pause_pipeline(app.get());
          }
          break;
        case 'r':
          for (auto& app : apps_) {
            resume_pipeline(app.get());
          }
          break;
        case 'q':
          quit_ = true;
          g_main_loop_quit(main_loop_);
          break;
        // Additional key commands (e.g., for selecting a tiled source)
        default:
          break;
      }
    }
    usleep(40000); // Sleep ~40 ms.
  }
}

//------------------------------------------------------------------------------
// xEventThread: processes X11 events (e.g., mouse clicks on the video window).
//------------------------------------------------------------------------------
void DeepStreamApplication::xEventThread() {
  std::lock_guard<std::mutex> lock(disp_mutex_);
  while (display_) {
    XEvent e;
    while (XPending(display_)) {
      XNextEvent(display_, &e);
      switch (e.type) {
        case ButtonPress: {
          // Handle mouse button events.
          // (Implement similar logic to the original C function nvds_x_event_thread.)
        } break;
        case KeyPress:
        case KeyRelease:
          // Optionally handle key events.
          break;
        case ClientMessage: {
          // Handle window close events.
          quit_ = true;
          g_main_loop_quit(main_loop_);
        } break;
        default:
          break;
      }
    }
    usleep(50000); // Sleep briefly.
  }
}

//------------------------------------------------------------------------------
// changeTerminalMode: enable or disable non-canonical terminal mode.
//------------------------------------------------------------------------------
void DeepStreamApplication::changeTerminalMode(bool enable) {
  if (enable) {
    if (!termios_saved_) {
      tcgetattr(STDIN_FILENO, &old_termios_);
      termios_saved_ = true;
    }
    termios newt = old_termios_;
    newt.c_lflag &= ~(ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
  } else {
    if (termios_saved_) {
      tcsetattr(STDIN_FILENO, TCSANOW, &old_termios_);
      termios_saved_ = false;
    }
  }
}

//------------------------------------------------------------------------------
// printRuntimeCommands: prints the available commands to the user.
//------------------------------------------------------------------------------
void DeepStreamApplication::printRuntimeCommands() {
  std::cout << "\nRuntime commands:\n"
            << "\th: Print this help\n"
            << "\tq: Quit\n\n"
            << "\tp: Pause\n"
            << "\tr: Resume\n\n";
  // Additional instructions for tiled display if enabled.
  if (apps_[0]->config.tiled_display_config.enable) {
    std::cout << "NOTE: To expand a source in the 2D tiled display and view object details, "
                 "left-click on the source.\n"
              << "      To go back to the tiled display, right-click anywhere on the window.\n\n";
  }
}

//------------------------------------------------------------------------------
// Signal handler: static method that forwards to the instance.
//------------------------------------------------------------------------------
void DeepStreamApplication::signalHandler(int signum) {
  if (instance_) {
    instance_->handleInterrupt(signum);
  }
}

//------------------------------------------------------------------------------
// handleInterrupt: called when SIGINT is received.
//------------------------------------------------------------------------------
void DeepStreamApplication::handleInterrupt(int signum) {
  std::cout << "User interrupted (signal " << signum << ")" << std::endl;
  quit_ = true;
  g_main_loop_quit(main_loop_);
}

//------------------------------------------------------------------------------
// cleanup: destroys pipelines, windows, and frees resources.
//------------------------------------------------------------------------------
void DeepStreamApplication::cleanup() {
  // Destroy pipelines and windows.
  for (unsigned i = 0; i < num_instances_; i++) {
    if (apps_[i]) {
      destroy_pipeline(apps_[i].get());
    }
    std::lock_guard<std::mutex> lock(disp_mutex_);
    if (display_ && windows_[i]) {
      XDestroyWindow(display_, windows_[i]);
      windows_[i] = 0;
    }
  }
  if (display_) {
    XCloseDisplay(display_);
    display_ = nullptr;
  }
  if (main_loop_) {
    g_main_loop_unref(main_loop_);
    main_loop_ = nullptr;
  }
}

//------------------------------------------------------------------------------
// main: creates the DeepStreamApplication object and runs it.
//------------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  auto app = DeepStreamApplication(argc, argv);
  absl::Status status = app.run();
  if (!status.ok()) {
    std::cerr << status << std::endl;
    return status.raw_code();
  }
  return 0;
}
