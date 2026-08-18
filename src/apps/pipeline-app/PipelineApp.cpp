/* clang-format off */
#include "src/libs/common/Status.h"
/* clang-format on */

#include "PipelineApp.h"
#include "StitchFrameTimePlan.h"

#include <gstreamer-1.0/gst/gstelement.h>
#include "hstream/src/apps/apps-common/deepstream_config.h"
#include "hstream/src/apps/apps-common/deepstream_sources.h"
#include "hstream/src/libs/wireless/StreamControl.h"

#include <cuda_runtime_api.h>
#include <gst/gstbin.h>
#include <gst/video/video.h>
#include <gst/video/videooverlay.h>
#include <nvbufsurface.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <signal.h>
#include <string.h>
#include <sys/select.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "TensorRtModelCache.h"
#include "hstream/src/apps/apps-common/HmGpuPreview.h"
#include "hstream/src/apps/apps-common/deepstream_app_version.h"
#include "hstream/src/apps/apps-common/deepstream_common.h"
#include "hstream/src/apps/apps-common/deepstream_sinks.h"
#include "hstream/src/libs/assets/AssetManager.h"
#include "hstream/src/libs/camera/AutoFocus.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"

#include "absl/cleanup/cleanup.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "nvds_version.h"

namespace fs = std::filesystem;

//------------------------------------------------------------------------------
// Debug category definition.
GST_DEBUG_CATEGORY(NVDS_APP);

namespace {

struct StitchFrameRewindRequest {
  long stage;
  uint64_t main_loop_generation;
};

absl::StatusOr<uint64_t> parse_time_option(const char* option, const char* value) {
  try {
    return hm::hhmmss_to_nanoseconds(value ? value : "");
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(
        std::string("Invalid ") + option + " value '" + (value ? value : "") + "': " + error.what());
  }
}

absl::StatusOr<uint64_t> parse_stitch_frame_time_option(const char* option, const char* value) {
  try {
    return hm::stitch_frame_time_to_nanoseconds(value ? value : "");
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(
        std::string("Invalid ") + option + " value '" + (value ? value : "") + "': " + error.what());
  }
}

std::string normalized_stitch_frame_time_config_value(uint64_t nanoseconds) {
  if (nanoseconds == 0) {
    return {};
  }
  const uint64_t total_milliseconds = nanoseconds / GST_MSECOND;
  const uint64_t hours = total_milliseconds / (60 * 60 * 1000);
  const uint64_t minutes = total_milliseconds / (60 * 1000) % 60;
  const uint64_t seconds = total_milliseconds / 1000 % 60;
  const uint64_t milliseconds = total_milliseconds % 1000;
  std::ostringstream value;
  value << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2) << minutes << ':' << std::setw(2)
        << seconds;
  if (milliseconds != 0) {
    value << '.' << std::setw(3) << milliseconds;
  }
  return value.str();
}

absl::StatusOr<uint64_t> configured_stitch_frame_time(const YAML::Node& config) {
  const auto value = hm::get_node(config, "stitching.stitch_frame_time");
  if (!value.has_value()) {
    return 0;
  }
  if (!value->IsScalar()) {
    return absl::InvalidArgumentError("stitching.stitch_frame_time must be a scalar HH:MM:SS or HH:MM:SS.mmm value");
  }
  try {
    return hm::stitch_frame_time_to_nanoseconds(value->as<std::string>());
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(TO_STRING("Invalid stitching.stitch_frame_time: " << error.what()));
  }
}

absl::StatusOr<std::optional<double>> active_stitch_output_rotation(const YAML::Node& config) {
  try {
    const auto stitcher = hm::get_node(config, "pipeline.hmstitcher");
    if (!stitcher.has_value() || !stitcher->IsMap() ||
        !hm::get_node_value(config, "pipeline.hmstitcher.enable", false)) {
      return std::nullopt;
    }
    double rotation = hm::get_node_value(config, "stitching.post_stitch_rotate_degrees", 0.0);
    rotation = hm::get_node_value(config, "pipeline.hmstitcher.post-stitch-rotate-degrees", rotation);
    if (!std::isfinite(rotation)) {
      return absl::InvalidArgumentError("Active hmstitcher post-stitch rotation must be finite");
    }
    return rotation == 0.0 ? 0.0 : rotation;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(TO_STRING("Invalid active hmstitcher rotation: " << error.what()));
  }
}

void emit_ui_startup(const char* stage, const char* message) {
  if (!g_getenv("HSTREAM_UI_PARENT_PID")) {
    return;
  }
  g_print("HSTREAM_STARTUP stage=%s message=%s\n", stage, message);
  std::fflush(stdout);
}

std::vector<std::string> normalize_cli_args(int argc, char* argv[]) {
  std::vector<std::string> args;
  args.reserve(argc);
  for (int i = 0; i < argc; ++i) {
    std::string arg = argv[i] ? argv[i] : "";
    if (arg.rfind("-t=", 0) == 0) {
      arg = "--time-limit=" + arg.substr(3);
    }
    args.push_back(std::move(arg));
  }
  return args;
}

std::vector<char*> make_mutable_argv(std::vector<std::string>& args) {
  std::vector<char*> mutable_argv;
  mutable_argv.reserve(args.size());
  for (std::string& arg : args) {
    mutable_argv.push_back(arg.data());
  }
  return mutable_argv;
}

std::string host_arch_name() {
#if defined(__x86_64__)
  return "x86_64";
#elif defined(__aarch64__)
  return "aarch64";
#else
  return "unknown";
#endif
}

bool manages_its_own_window(GstElement* sink) {
  if (sink == nullptr) {
    return false;
  }
  GstElementFactory* factory = gst_element_get_factory(sink);
  return factory != nullptr &&
      g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)), NVDS_ELEM_SINK_3D) == 0;
}

absl::Status pause_pipeline_for_model_initialization(
    AppCtx* app_ctx,
    const volatile sig_atomic_t* interrupt_requested) {
  GstElement* pipeline = app_ctx ? app_ctx->pipeline.pipeline : nullptr;
  if (!pipeline) {
    return absl::InvalidArgumentError("Missing pipeline while initializing models");
  }
  if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
    return absl::InternalError("Failed to set pipeline to PAUSED");
  constexpr auto kModelInitializationTimeout = std::chrono::minutes(15);
  constexpr auto kCancellationTimeout = std::chrono::seconds(5);
  constexpr GstClockTime kPollInterval = 100 * GST_MSECOND;
  const auto initialization_deadline = std::chrono::steady_clock::now() + kModelInitializationTimeout;
  std::optional<std::chrono::steady_clock::time_point> cancellation_deadline;
  while (true) {
    const GstStateChangeReturn result = gst_element_get_state(pipeline, nullptr, nullptr, kPollInterval);
    const bool interrupted = interrupt_requested && *interrupt_requested;
    if (interrupted && !cancellation_deadline.has_value()) {
      g_printerr("Interrupt received during pipeline preroll; cancelling stitching calibration\n");
      if (GstElement* stitcher = app_ctx->pipeline.hmstitcher_bin.elem_hmstitcher) {
        g_object_set(G_OBJECT(stitcher), "cancel-pending-work", TRUE, nullptr);
      }
      cancellation_deadline = std::chrono::steady_clock::now() + kCancellationTimeout;
    }
    if (interrupted && result != GST_STATE_CHANGE_ASYNC) {
      return absl::CancelledError("Pipeline initialization interrupted");
    }
    if (result == GST_STATE_CHANGE_FAILURE) {
      return absl::InternalError("Pipeline failed while initializing models");
    }
    if (result != GST_STATE_CHANGE_ASYNC) {
      return absl::OkStatus();
    }
    const auto now = std::chrono::steady_clock::now();
    if (cancellation_deadline.has_value() && now >= *cancellation_deadline) {
      g_printerr("Calibration did not acknowledge cancellation within five seconds; terminating the worker process\n");
      std::fflush(stderr);
      std::_Exit(128 + SIGINT);
    }
    if (now >= initialization_deadline) {
      return absl::DeadlineExceededError("Timed out waiting for pipeline model initialization");
    }
  }
}

void prepend_env_path(const char* name, const fs::path& dir) {
  if (dir.empty() || !fs::is_directory(dir)) {
    return;
  }
  const std::string dir_str = dir.string();
  const char* existing_env = std::getenv(name);
  if (!existing_env || std::string(existing_env).empty()) {
    setenv(name, dir_str.c_str(), 1);
    return;
  }
  const std::string existing(existing_env);
  for (const auto& part : absl::StrSplit(existing, ':')) {
    if (part == dir_str) {
      return;
    }
  }
  const std::string updated = dir_str + ":" + existing;
  setenv(name, updated.c_str(), 1);
}

std::optional<fs::path> runtime_root_from_executable(const char* argv0) {
  std::error_code ec;
  fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (ec && argv0 && std::string(argv0).find('/') != std::string::npos) {
    exe = fs::canonical(argv0, ec);
  }
  if (ec || exe.empty()) {
    return std::nullopt;
  }
  for (fs::path cursor = exe.parent_path(); !cursor.empty(); cursor = cursor.parent_path()) {
    if (cursor.filename() == "bazel-bin") {
      return cursor.parent_path();
    }
    if (cursor.filename() == "bin" && fs::exists(cursor.parent_path() / "configs")) {
      return cursor.parent_path();
    }
    if (cursor == cursor.root_path()) {
      break;
    }
  }
  return std::nullopt;
}

fs::path pipeline_runtime_root(const char* argv0) {
  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  if (!ec && (fs::is_directory(cwd / "bazel-bin/src/gst-plugins") || fs::is_directory(cwd / "lib/gst-plugins"))) {
    return cwd;
  }
  if (auto exe_root = runtime_root_from_executable(argv0)) {
    return *exe_root;
  }
  return cwd;
}

void stage_bazel_gst_plugins(const fs::path& root) {
  const fs::path bazel_plugin_root = root / "bazel-bin/src/gst-plugins";
  if (!fs::is_directory(bazel_plugin_root)) {
    return;
  }

  std::error_code ec;
  const fs::path runtime_plugin_dir = root / ".cache/gst-plugin-path" / host_arch_name();
  fs::create_directories(runtime_plugin_dir, ec);
  if (ec) {
    return;
  }
  for (const fs::directory_entry& entry : fs::directory_iterator(runtime_plugin_dir, ec)) {
    if (!ec && entry.path().extension() == ".so" && fs::is_symlink(entry.symlink_status())) {
      fs::remove(entry.path(), ec);
    }
  }

  for (const fs::directory_entry& entry : fs::recursive_directory_iterator(bazel_plugin_root, ec)) {
    if (ec || !entry.is_regular_file()) {
      continue;
    }
    const fs::path path = entry.path();
    const std::string path_str = path.string();
    const std::string filename = path.filename().string();
    if (path_str.find("/testutils/") != std::string::npos || path_str.find(".runfiles/") != std::string::npos) {
      continue;
    }
    if (path.extension() == ".so") {
      prepend_env_path("LD_LIBRARY_PATH", path.parent_path());
    }
    if ((filename.rfind("libnvdsgst_", 0) == 0 || filename.rfind("libgst", 0) == 0) && path.extension() == ".so") {
      std::error_code link_ec;
      const fs::path canonical = fs::canonical(path, link_ec);
      if (link_ec) {
        continue;
      }
      const fs::path link_path = runtime_plugin_dir / path.filename();
      fs::remove(link_path, link_ec);
      fs::create_symlink(canonical, link_path, link_ec);
    }
  }
  prepend_env_path("GST_PLUGIN_PATH", runtime_plugin_dir);
}

void stage_bazel_runtime_libraries(const fs::path& root) {
  const fs::path bazel_bin = root / "bazel-bin";
  if (!fs::is_directory(bazel_bin))
    return;
  std::error_code ec;
  fs::path onnxruntime;
  for (const fs::directory_entry& solib : fs::directory_iterator(bazel_bin, ec)) {
    if (ec)
      return;
    if (!solib.is_directory(ec) || solib.path().filename().string().rfind("_solib_", 0) != 0) {
      ec.clear();
      continue;
    }
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(solib.path(), ec)) {
      if (ec)
        return;
      if (entry.path().filename() == "libonnxruntime.so.1" && entry.is_regular_file(ec) && !ec) {
        onnxruntime = fs::canonical(entry.path(), ec);
        break;
      }
    }
    if (!onnxruntime.empty())
      break;
  }
  if (onnxruntime.empty() || ec)
    return;
  const fs::path runtime_dir = root / ".cache/runtime-lib-path" / host_arch_name();
  fs::create_directories(runtime_dir, ec);
  if (ec)
    return;
  const fs::path link = runtime_dir / "libonnxruntime.so.1";
  fs::remove(link, ec);
  ec.clear();
  fs::create_symlink(onnxruntime, link, ec);
  if (!ec)
    prepend_env_path("LD_LIBRARY_PATH", runtime_dir);
}

void configure_pipeline_runtime_environment(const char* argv0) {
  const fs::path root = pipeline_runtime_root(argv0);
  const fs::path packaged_native_models = root / "pretrained/native-calibration";
  if (!std::getenv("HM_NATIVE_MODEL_DIR") && fs::is_directory(packaged_native_models)) {
    setenv("HM_NATIVE_MODEL_DIR", packaged_native_models.c_str(), 1);
  }
  stage_bazel_runtime_libraries(root);
  std::error_code ec;
  fs::path registry_dir = root / ".cache/gstreamer-1.0";
  fs::create_directories(registry_dir, ec);
  if (ec) {
    if (const char* home = std::getenv("HOME"); home && *home) {
      registry_dir = fs::path(home) / ".cache/gstreamer-1.0";
      ec.clear();
      fs::create_directories(registry_dir, ec);
    }
  }
  if (!ec) {
    const std::string registry =
        (registry_dir / ("registry.hstream.native-onnx-v1." + host_arch_name() + ".bin")).string();
    setenv("GST_REGISTRY", registry.c_str(), 1);
  }

  prepend_env_path("GST_PLUGIN_PATH", root / "lib/gst-plugins");
  prepend_env_path("GST_PLUGIN_PATH", "/opt/nvidia/deepstream/deepstream/lib/gst-plugins");
  prepend_env_path("LD_LIBRARY_PATH", root / "lib");
  prepend_env_path("LD_LIBRARY_PATH", root / "lib/gst-plugins");
  prepend_env_path("LD_LIBRARY_PATH", "/opt/nvidia/deepstream/deepstream/lib");
  prepend_env_path("LD_LIBRARY_PATH", "/opt/nvidia/deepstream/deepstream/lib/gst-plugins");
  stage_bazel_gst_plugins(root);

  const fs::path yolo_so = root / "bazel-bin/src/libs/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so";
  if (fs::exists(yolo_so)) {
    const fs::path lib_dir = root / "lib";
    fs::create_directories(lib_dir, ec);
    const fs::path link_path = lib_dir / "libnvdsinfer_custom_impl_Yolo.so";
    if (!fs::exists(link_path) || fs::is_symlink(fs::symlink_status(link_path))) {
      std::error_code link_ec;
      const fs::path canonical = fs::canonical(yolo_so, link_ec);
      if (!link_ec) {
        fs::remove(link_path, link_ec);
        fs::create_symlink(canonical, link_path, link_ec);
      }
    }
  }
}

std::string format_duration_ns(uint64_t ns) {
  if (ns == GST_CLOCK_TIME_NONE) {
    return "--:--:--";
  }
  uint64_t total_seconds = ns / GST_SECOND;
  const uint64_t hours = total_seconds / 3600;
  total_seconds %= 3600;
  const uint64_t minutes = total_seconds / 60;
  const uint64_t seconds = total_seconds % 60;

  std::ostringstream out;
  out << std::setfill('0') << std::setw(2) << hours << ":" << std::setw(2) << minutes << ":" << std::setw(2) << seconds;
  return out.str();
}

std::string format_progress_bar(double fraction, size_t width = 24) {
  if (!std::isfinite(fraction)) {
    fraction = 0.0;
  }
  fraction = std::clamp(fraction, 0.0, 1.0);
  const size_t filled = static_cast<size_t>(std::round(fraction * static_cast<double>(width)));
  std::string bar;
  bar.reserve(width + 2);
  bar.push_back('[');
  for (size_t i = 0; i < width; ++i) {
    bar.push_back(i < filled ? '=' : '-');
  }
  bar.push_back(']');
  return bar;
}

std::string format_fixed(double value, int precision = 2) {
  if (!std::isfinite(value)) {
    return "--";
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(precision) << value;
  return out.str();
}

std::vector<std::string> split_uri_list(const gchar* uri_list) {
  std::vector<std::string> uris;
  if (!uri_list || !*uri_list) {
    return uris;
  }
  for (absl::string_view item : absl::StrSplit(uri_list, ';', absl::SkipEmpty())) {
    std::string uri(item);
    uri.erase(uri.begin(), std::find_if(uri.begin(), uri.end(), [](unsigned char c) { return !std::isspace(c); }));
    uri.erase(
        std::find_if(uri.rbegin(), uri.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), uri.end());
    if (!uri.empty()) {
      uris.emplace_back(std::move(uri));
    }
  }
  return uris;
}

bool parse_finite_double(const char* value, double& parsed) {
  if (!value || !*value) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  parsed = std::strtod(value, &end);
  if (value == end || errno == ERANGE || !std::isfinite(parsed)) {
    return false;
  }
  while (end && *end && std::isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  return end && *end == '\0';
}

std::optional<std::string> file_uri_to_path(const char* uri) {
  if (!uri || !*uri || !g_str_has_prefix(uri, "file://")) {
    return std::nullopt;
  }
  GError* error = nullptr;
  gchar* filename = g_filename_from_uri(uri, nullptr, &error);
  if (error) {
    g_error_free(error);
  }
  if (!filename) {
    return std::nullopt;
  }
  std::string path(filename);
  g_free(filename);
  return path;
}

uint64_t duration_for_file_uri_ns(const char* uri) {
  std::optional<std::string> path = file_uri_to_path(uri);
  if (!path) {
    return GST_CLOCK_TIME_NONE;
  }
  hm::Videoinfo info = hm::getVideoInfo(*path);
  if (info.fps <= 0.0 || info.frame_count == 0) {
    return GST_CLOCK_TIME_NONE;
  }
  const double seconds = static_cast<double>(info.frame_count) / info.fps;
  if (!std::isfinite(seconds) || seconds <= 0.0) {
    return GST_CLOCK_TIME_NONE;
  }
  return static_cast<uint64_t>(seconds * static_cast<double>(GST_SECOND));
}

uint64_t duration_for_source_ns(const NvDsSourceConfig& source_config) {
  std::vector<std::string> uris = split_uri_list(source_config.uri_list);
  if (uris.empty() && source_config.uri && *source_config.uri) {
    uris.emplace_back(source_config.uri);
  }
  if (uris.empty()) {
    return GST_CLOCK_TIME_NONE;
  }

  uint64_t total_ns = 0;
  for (const std::string& uri : uris) {
    uint64_t duration_ns = duration_for_file_uri_ns(uri.c_str());
    if (duration_ns == GST_CLOCK_TIME_NONE) {
      return GST_CLOCK_TIME_NONE;
    }
    total_ns += duration_ns;
  }
  return total_ns;
}

uint64_t hmstitcher_source_offset_ns(const HmStitcherConfig& stitcher_config, guint source_index) {
  if (source_index == 0) {
    return stitcher_config.left_frame_offset_ns;
  }
  if (source_index == 1) {
    return stitcher_config.right_frame_offset_ns;
  }
  return 0;
}

uint64_t subtract_duration_ns(uint64_t duration_ns, uint64_t offset_ns) {
  if (duration_ns == GST_CLOCK_TIME_NONE) {
    return GST_CLOCK_TIME_NONE;
  }
  return duration_ns > offset_ns ? duration_ns - offset_ns : 0;
}

} // namespace

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
      show_(FALSE),
      show_bbox_text_(FALSE),
      print_dependencies_version_(FALSE),
      quit_(FALSE),
      dump_pipeline_dot_(FALSE),
      force_reconfigure_(FALSE),
      return_value_(0),
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
  instance_ = this;
  // const char* plugin_path = getenv("GST_PLUGIN_PATH");
  // if (plugin_path) {
  //   std::cout << "GST_PLUGIN_PATH=\"" << plugin_path << "\"\n" << std::flush;
  // }
}

//------------------------------------------------------------------------------
// PipelineApplication destructor.
PipelineApplication::~PipelineApplication() {}

//------------------------------------------------------------------------------
// Callback and helper functions implementation.
// (The following implementations are largely as provided in the original source.)
//------------------------------------------------------------------------------

absl::Status PipelineApplication::initializeInstances(CleanupStack& /*cleanup_stack*/) {
  // Section 1: Create and initialize each HmApp instance.
  int i = -1;
  while (cfg_files_[++i]) {
    // TODO: override_gpu_id_ could be a list/vector of them to use with each config file
    // or pipeline-options can set each one...
    auto app_ctx = std::make_unique<HmApp>(game_id_ ? *game_id_ : "", cfg_files_[i], override_gpu_id_);
    // app_ctx = std::make_unique<HmApp>(game_id_ ? *game_id_ : "");
    app_ctx->person_class_id = -1;
    app_ctx->car_class_id = -1;
    app_ctx->index = i;
    app_ctx->active_source_index = -1;
    app_ctx->element_message_cb = handle_element_message_static;
    if (show_bbox_text_)
      app_ctx->show_bbox_text = TRUE;
    if (input_uris_ && input_uris_[i]) {
      app_ctx->config.multi_source_config[0].uri = g_strdup_printf("%s", input_uris_[i]);
      g_free(input_uris_[i]);
    }
    HM_RETURN_IF_ERROR(app_ctx->load_config());
    long stage = 0;
    if (g_str_has_suffix(app_ctx->app_config_file().c_str(), ".yml") ||
        g_str_has_suffix(app_ctx->app_config_file().c_str(), ".yaml")) {
      YAML::Node app_config;
      HM_ASSIGN_OR_RETURN(app_config, get_app_config(app_ctx->app_config_file().c_str()));
      // Support both the legacy top-level `stage` key and DeepStream-style `application.stage`.
      stage = hm::get_node_value(app_config, "application.stage", stage);
      stage = hm::get_node_value(app_config, "stage", stage);
    }
    stage_app_contexts_[stage].emplace_back(std::move(app_ctx));
  }
  if (!stage_app_contexts_.empty()) {
    current_stage_ = stage_app_contexts_.begin()->first;
  }
  return absl::OkStatus();
}

absl::Status PipelineApplication::configureInstances(
    size_t stage_index,
    std::vector<std::shared_ptr<HmApp>>& app_contexts) {
  std::vector<std::shared_ptr<HmApp>> valid_app_contexts;
  std::vector<double> stage_stitch_output_rotations;
  for (size_t i = 0; i < app_contexts.size(); ++i) {
    auto& app_ctx = app_contexts[i];
    if (g_str_has_suffix(app_ctx->app_config_file().c_str(), ".yml") ||
        g_str_has_suffix(app_ctx->app_config_file().c_str(), ".yaml")) {
      if (!app_ctx->underlay_config("pipeline", app_ctx->app_config_file())) {
        NVGSTDS_ERR_MSG_V("Failed to merge in config file '%s'", app_ctx->app_config_file().c_str());
        app_ctx->return_value = -1;
        return absl::InternalError("Failed to merge in config file");
      }

      // Run enable-source-types in case we have any subconfigs that have 'type' set
      if (!enabled_source_types_.empty()) {
        if (stage_index) {
          if (enabled_source_types_.size() != 1 && stage_index >= enabled_source_types_.size()) {
            return absl::InvalidArgumentError(TO_STRING(
                "Number of 'enabled source types' must be zero, one, or at least as "
                << "many as the number of stages, or else it's not clear what to apply to this stage " << stage_index));
          }
        }

        app_ctx->configurator().enable_sections(
            "source",
            "type",
            enabled_source_types_.size() == 1 ? enabled_source_types_.at(0) : enabled_source_types_.at(stage_index),
            /*disable_others=*/true,
            "source-id");
      }

      if (!enabled_sink_types_.empty()) {
        if (stage_index) {
          if (enabled_sink_types_.size() != 1 && stage_index >= enabled_sink_types_.size()) {
            return absl::InvalidArgumentError(TO_STRING(
                "Number of 'enabled sink types' must be zero, one, or at least as "
                << "many as the number of stages, or else it's not clear what to apply to this stage " << stage_index));
          }
        }

        app_ctx->configurator().enable_sections(
            "sink",
            "type",
            enabled_sink_types_.size() == 1 ? enabled_sink_types_.at(0) : enabled_sink_types_.at(stage_index),
            /*disable_others=*/true,
            "sink-id");
      }

      HM_RETURN_IF_ERROR(app_ctx->configurator().load_sub_configs(
          "pipeline", {"source", "sink"}, fs::path(app_ctx->app_config_file()).parent_path().string()));

      // Run enable-source-types again
      // if (!enabled_source_types_.empty()) {
      //   app_ctx->configurator().enable_source_types(enabled_source_types_, true);
      // }

      // Finally, command-line config overrides (pipeline or otherwise)
      if (!pipeline_options_.empty()) {
        // Historically we avoided applying pipeline options to stage -1 (stitch config), but we do want them for the
        // main stage (stage >= 0). This also ensures single-stage runs (only stage 0) get CLI options.
        if (current_stage_ >= 0) {
          for (const std::map<std::string, std::string>& options : pipeline_options_) {
            for (const auto& kv_item : options) {
              HM_RETURN_IF_ERROR(app_ctx->configurator().apply_config_item(kv_item.first, kv_item.second));
            }
          }
        }
      }

      // Now auto-configure stuff as needed, i.e. dependent pipelines or stitching (if needed)
      absl::Status configuration_status = app_ctx->complete_configuration(
          force_reconfigure_,
          clean_stitching_artifacts_,
          clean_stitching_from_control_points_,
          clean_stitching_expected_invalidation_id_ ? clean_stitching_expected_invalidation_id_ : "",
          show_ || show_render_scale_ == 0.0,
          show_render_scale_);
      if (configuration_status.code() == absl::StatusCode::kCancelled) {
        if (!clean_stitching_artifacts_ && !clean_stitching_from_control_points_) {
          return configuration_status;
        }
        std::cerr << configuration_status << std::endl;
        continue;
      }
      if (!configuration_status.ok()) {
        return configuration_status;
      }
      if (stitch_frame_time_set_ && app_ctx->configurator().stitching_calibration_required()) {
        HM_RETURN_IF_ERROR(
            app_ctx->configurator().persist_stitch_frame_time_override(stitch_frame_time_override_config_value_));
      }
      std::optional<double> stitch_output_rotation;
      HM_ASSIGN_OR_RETURN(stitch_output_rotation, active_stitch_output_rotation(app_ctx->configurator().config()));
      if (stitch_output_rotation.has_value()) {
        stage_stitch_output_rotations.push_back(*stitch_output_rotation);
        if (!hm::pipeline_internal::stitch_output_rotations_are_consistent(stage_stitch_output_rotations)) {
          return absl::InvalidArgumentError(
              "All active hmstitcher instances in a stage must use the same post-stitch rotation");
        }
      }
      const std::string& active_invalidation_id = app_ctx->configurator().active_stitching_invalidation_id();
      if (!active_invalidation_id.empty()) {
        const char* runtime_invalidation_id = g_getenv("HSTREAM_CALIBRATION_INVALIDATION_ID");
        if (runtime_invalidation_id && *runtime_invalidation_id && runtime_invalidation_id != active_invalidation_id) {
          return absl::InvalidArgumentError(
              "All pipeline instances must use the same stitching calibration invalidation ID");
        }
        if (!g_setenv("HSTREAM_CALIBRATION_INVALIDATION_ID", active_invalidation_id.c_str(), /*overwrite=*/TRUE)) {
          return absl::InternalError("Unable to publish the saved stitching invalidation ID to runtime plugins");
        }
      }
      if (app_ctx->configurator().stitching_calibration_required()) {
        HM_RETURN_IF_ERROR(app_ctx->configurator().apply_config_item(
            "pipeline.hmstitcher.private-properties.calibration-run-generation",
            std::to_string(main_loop_generation_ + 1)));
      }
      YAML::Node config = app_ctx->configurator().config();
      if (!stitch_frame_time_set_) {
        uint64_t configured_stitch_frame_time_ns = 0;
        HM_ASSIGN_OR_RETURN(configured_stitch_frame_time_ns, configured_stitch_frame_time(config));
        if (stitch_frame_time_loaded_from_config_ && stitch_frame_time_ns_ != configured_stitch_frame_time_ns) {
          return absl::InvalidArgumentError(
              "All pipeline instances must use the same stitching.stitch_frame_time value");
        }
        stitch_frame_time_ns_ = configured_stitch_frame_time_ns;
        stitch_frame_time_loaded_from_config_ = true;
      }
      // std::cout << config["pipeline"] << "\n";
      if (!config["pipeline"].IsDefined()) {
        NVGSTDS_ERR_MSG_V("Config file '%s' did not produce a pipeline section", app_ctx->app_config_file().c_str());
        app_ctx->return_value = -1;
        return absl::InternalError("Failed to parse config file");
      }
      emit_ui_startup("models", "Preparing TensorRT models and engine caches");
      HM_RETURN_IF_ERROR(
          hm::pipeline::PrepareTensorRtModelCache(
              config["pipeline"], fs::path(app_ctx->app_config_file()).parent_path()));
      if (!parse_config_yaml(
              config["pipeline"], &app_ctx->config, fs::path(app_ctx->app_config_file()).parent_path())) {
        NVGSTDS_ERR_MSG_V("Failed to parse config file '%s'", app_ctx->app_config_file().c_str());
        app_ctx->return_value = -1;
        return absl::InternalError("Failed to parse config file");
      }
    } else if (g_str_has_suffix(app_ctx->app_config_file().c_str(), ".txt")) {
      if (!parse_config_file(&app_ctx->config, app_ctx->app_config_file().c_str())) {
        NVGSTDS_ERR_MSG_V("Failed to parse config file '%s'", app_ctx->app_config_file().c_str());
        app_ctx->return_value = -1;
        return absl::InternalError("Failed to parse config file");
      }
    }
    const bool launched_by_ui = std::getenv("HSTREAM_UI_PARENT_PID") != nullptr;
    if (hm::playback_progress_sampling_enabled(app_ctx->config.enable_perf_measurement, launched_by_ui)) {
      app_ctx->config.enable_perf_measurement = TRUE;
      app_ctx->config.perf_measurement_interval_sec = std::max(1U, app_ctx->config.perf_measurement_interval_sec);
      if (launched_by_ui) {
        app_ctx->config.perf_measurement_interval_sec = std::min(5U, app_ctx->config.perf_measurement_interval_sec);
      }
    }
    if (!ui_preview_window_ids_.empty()) {
      app_ctx->config.hmsticher_config.ui_preview = TRUE;
    }
    valid_app_contexts.emplace_back(std::move(app_ctx));
  }
  app_contexts = std::move(valid_app_contexts);
  if (progress_ui_ && progress_ui_->started()) {
    progress_ui_->setGraphSnapshot(build_progress_graph_snapshot(app_contexts));
  }
  return absl::OkStatus();
}

absl::Status PipelineApplication::createPipelines(
    std::vector<std::shared_ptr<HmApp>>& app_contexts,
    CleanupStack& cleanup_stack) {
  // Section 2: Create pipelines for each instance.
  for (guint i = 0; i < app_contexts.size(); i++) {
    if (!create_pipeline(app_contexts[i].get(), nullptr, all_bbox_generated, perf_cb_static, overlay_graphics_static)) {
      NVGSTDS_ERR_MSG_V("Failed to create pipeline");
      return absl::InternalError("Failed to create pipeline");
    }
    const uint64_t initial_position_ns = initial_pipeline_position_ns(app_contexts[i].get());
    HM_RETURN_IF_ERROR(
        app_contexts[i]->configurator().prepare_initial_pipeline_position(
            app_contexts[i]->pipeline, app_contexts[i]->config, initial_position_ns));
    if (dump_pipeline_dot_) {
      std::string s = "pipeline";
      if (i) {
        s += '_';
        s += std::to_string(i);
      }
      hm::save_dot_file(app_contexts[i]->pipeline.pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "pipeline_created" + s);
    }
  }
  HM_RETURN_IF_ERROR(configure_source_preview_sinks(app_contexts));
  return auto_focus_cameras(app_contexts);
}

absl::Status PipelineApplication::configure_source_preview_sinks(
    const std::vector<std::shared_ptr<HmApp>>& app_contexts) {
  if (!ui_preview_window_ids_.empty()) {
    if (app_contexts.size() != 1) {
      return absl::InvalidArgumentError("GPU-native UI previews require exactly one active pipeline context");
    }
    constexpr guint64 kPreviewImageBudgetBytes = 64ULL * 1024ULL * 1024ULL;
    guint64 preview_image_bytes = 0;

    struct PreviewProbeState {
      GstElement* sink{nullptr};
      GstClockTime last_pts{GST_CLOCK_TIME_NONE};
      std::chrono::steady_clock::time_point last_emission;
      bool have_last_emission{false};
    };
    auto preview_probe = +[](GstPad*, GstPadProbeInfo* info, gpointer user_data) -> GstPadProbeReturn {
      auto* state = static_cast<PreviewProbeState*>(user_data);
      if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) != 0) {
        GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
        if (event && GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
          GstCaps* caps = nullptr;
          gst_event_parse_caps(event, &caps);
          const GstStructure* structure = caps && gst_caps_get_size(caps) ? gst_caps_get_structure(caps, 0) : nullptr;
          gint width = 0;
          gint height = 0;
          if (structure && gst_structure_get_int(structure, "width", &width) &&
              gst_structure_get_int(structure, "height", &height) && width > 0 && height > 0) {
            hm::gpu_preview::set_source_geometry(state->sink, width, height);
          }
        }
      }
      if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
        const auto now = std::chrono::steady_clock::now();
        constexpr auto kMinimumWallPeriod = std::chrono::nanoseconds(GST_SECOND / 30);
        if (state->have_last_emission && now - state->last_emission < kMinimumWallPeriod)
          return GST_PAD_PROBE_DROP;
        GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        const GstClockTime pts = buffer ? GST_BUFFER_PTS(buffer) : GST_CLOCK_TIME_NONE;
        constexpr GstClockTime kMinimumPreviewPeriod = GST_SECOND / 30;
        if (GST_CLOCK_TIME_IS_VALID(pts) && GST_CLOCK_TIME_IS_VALID(state->last_pts) && pts >= state->last_pts &&
            pts - state->last_pts < kMinimumPreviewPeriod) {
          return GST_PAD_PROBE_DROP;
        }
        if (GST_CLOCK_TIME_IS_VALID(pts))
          state->last_pts = pts;
        state->last_emission = now;
        state->have_last_emission = true;
      }
      return GST_PAD_PROBE_OK;
    };

    auto configure_path = [&](GstElement* queue,
                              GstElement* ingress_isolation,
                              GstElement* isolation,
                              GstElement* converter,
                              GstElement* caps_filter,
                              GstElement* sink,
                              const std::string& channel,
                              guint64 window_id,
                              guint gpu_id,
                              gint width,
                              gint height,
                              bool link_elements) -> bool {
      if (!queue || !ingress_isolation || !isolation || !converter || !caps_filter || !sink)
        return false;
      const guint64 aligned_pitch = (static_cast<guint64>(width) * 4ULL + 255ULL) & ~255ULL;
      const guint64 path_image_bytes = aligned_pitch * static_cast<guint64>(height) * 2ULL;
      if (path_image_bytes > kPreviewImageBudgetBytes - preview_image_bytes)
        return false;
      preview_image_bytes += path_image_bytes;
      g_object_set(
          G_OBJECT(queue),
          "leaky",
          2,
          "max-size-buffers",
          1,
          "max-size-bytes",
          0,
          "max-size-time",
          static_cast<guint64>(0),
          nullptr);
      const bool initially_active = channel == active_ui_preview_channel_;
      g_object_set(
          G_OBJECT(ingress_isolation),
          "channel",
          channel.c_str(),
          "generation",
          static_cast<guint64>(active_ui_preview_generation_),
          "active",
          initially_active ? TRUE : FALSE,
          nullptr);
      g_object_set(
          G_OBJECT(isolation),
          "channel",
          channel.c_str(),
          "generation",
          static_cast<guint64>(active_ui_preview_generation_),
          "active",
          initially_active ? TRUE : FALSE,
          nullptr);
      hm::gpu_preview::set_isolation_failure_peer(isolation, ingress_isolation);
      g_object_set(
          G_OBJECT(converter),
          "gpu-id",
          gpu_id,
          "nvbuf-memory-type",
          NVBUF_MEM_CUDA_DEVICE,
          "output-buffers",
          1,
          nullptr);
      GstCaps* caps = gst_caps_new_simple(
          "video/x-raw",
          "format",
          G_TYPE_STRING,
          "RGBA",
          "width",
          G_TYPE_INT,
          width,
          "height",
          G_TYPE_INT,
          height,
          nullptr);
      gst_caps_set_features(caps, 0, gst_caps_features_new("memory:NVMM", nullptr));
      g_object_set(G_OBJECT(caps_filter), "caps", caps, nullptr);
      gst_caps_unref(caps);
      g_object_set(
          G_OBJECT(sink),
          "window-id",
          window_id,
          "gpu-id",
          gpu_id,
          "channel",
          channel.c_str(),
          "generation",
          static_cast<guint64>(active_ui_preview_generation_),
          "sync",
          FALSE,
          "async",
          FALSE,
          "qos",
          FALSE,
          "enable-last-sample",
          FALSE,
          nullptr);
      // The ingress gate keeps inactive buffers out of the queue. The second
      // gate remains after the queue as a drain barrier: once it is closed,
      // no already-enqueued buffer can still reach the converter or renderer.
      if (link_elements &&
          !gst_element_link_many(ingress_isolation, queue, isolation, converter, caps_filter, sink, nullptr))
        return false;
      // Rate-limit before the queue so high-frame-rate inputs do not wake the
      // preview task for frames that will be discarded.
      GstPad* probe_pad = gst_element_get_static_pad(ingress_isolation, "src");
      if (!probe_pad)
        return false;
      auto* probe_state = new PreviewProbeState{GST_ELEMENT(gst_object_ref(sink))};
      gst_pad_add_probe(
          probe_pad,
          static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
          preview_probe,
          probe_state,
          +[](gpointer data) {
            auto* state = static_cast<PreviewProbeState*>(data);
            gst_object_unref(state->sink);
            delete state;
          });
      gst_object_unref(probe_pad);
      ui_preview_channels_[channel] = UiPreviewChannel{ingress_isolation, isolation, sink};
      if (initially_active)
        active_ui_preview_channel_ = channel;
      return true;
    };

    const auto& app_context = app_contexts.front();
    NvDsSinkBin& output = app_context->pipeline.instance_bins[0].sink_bin;
    const auto program_target = ui_preview_window_ids_.find("program");
    if (program_target != ui_preview_window_ids_.end()) {
      GstElement* queue = gst_element_factory_make(NVDS_ELEM_QUEUE, "program_gpu_preview_queue");
      GstElement* ingress_isolation =
          gst_element_factory_make("hmpreviewisolation", "program_gpu_preview_ingress_isolation");
      GstElement* isolation = gst_element_factory_make("hmpreviewisolation", "program_gpu_preview_isolation");
      GstElement* converter = gst_element_factory_make(NVDS_ELEM_VIDEO_CONV, "program_gpu_preview_converter");
      GstElement* caps_filter = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, "program_gpu_preview_caps");
      GstElement* sink = gst_element_factory_make("hmgpupreviewsink", "program_gpu_preview_sink");
      if (!output.bin || !output.tee || !queue || !ingress_isolation || !isolation || !converter || !caps_filter ||
          !sink) {
        return absl::InternalError("Could not create the GPU-native Program preview branch");
      }
      gst_bin_add_many(GST_BIN(output.bin), ingress_isolation, queue, isolation, converter, caps_filter, sink, nullptr);
      if (!configure_path(
              queue,
              ingress_isolation,
              isolation,
              converter,
              caps_filter,
              sink,
              "program",
              program_target->second,
              app_context->config.hmsticher_config.gpu_id,
              1600,
              900,
              true) ||
          !link_element_to_tee_src_pad(output.tee, ingress_isolation)) {
        return absl::InternalError("Could not link the GPU-native Program preview branch");
      }
    }

    const auto stitched_target = ui_preview_window_ids_.find("stitched");
    HmStitcherBin& stitcher = app_context->pipeline.hmstitcher_bin;
    if (stitched_target != ui_preview_window_ids_.end()) {
      if (!stitcher.preview_queue || !stitcher.preview_ingress_isolation || !stitcher.preview_isolation ||
          !stitcher.preview_converter || !stitcher.preview_caps_filter || !stitcher.preview_sink) {
        return absl::FailedPreconditionError("The stitched GPU preview branch was not created");
      }
      g_object_set(G_OBJECT(stitcher.preview_sink), "window-id", stitched_target->second, nullptr);
      if (!configure_path(
              stitcher.preview_queue,
              stitcher.preview_ingress_isolation,
              stitcher.preview_isolation,
              stitcher.preview_converter,
              stitcher.preview_caps_filter,
              stitcher.preview_sink,
              "stitched",
              stitched_target->second,
              app_context->config.hmsticher_config.gpu_id,
              1600,
              900,
              false)) {
        return absl::InternalError("Could not configure the GPU-native Stitched preview branch");
      }
    }

    NvDsSrcParentBin& sources = app_context->pipeline.multi_src_bin;
    for (guint source_index = 0; source_index < sources.num_bins; ++source_index) {
      const std::string channel = "source" + std::to_string(source_index);
      const auto target = ui_preview_window_ids_.find(channel);
      if (target == ui_preview_window_ids_.end())
        continue;
      NvDsSrcBin& source = sources.sub_bins[source_index];
      if (!source.bin || !source.tee || !source.fakesink_queue || !source.fakesink) {
        return absl::FailedPreconditionError(TO_STRING(channel << " does not expose a preview tee branch"));
      }
      GstPad* source_queue_sink = gst_element_get_static_pad(source.fakesink_queue, "sink");
      GstPad* old_tee_pad = source_queue_sink ? gst_pad_get_peer(source_queue_sink) : nullptr;
      if (!source_queue_sink || !old_tee_pad || GST_OBJECT_PARENT(old_tee_pad) != GST_OBJECT(source.tee) ||
          !gst_pad_unlink(old_tee_pad, source_queue_sink)) {
        if (old_tee_pad)
          gst_object_unref(old_tee_pad);
        if (source_queue_sink)
          gst_object_unref(source_queue_sink);
        return absl::InternalError(TO_STRING("Could not detach the fake preview branch for " << channel));
      }
      gst_element_release_request_pad(source.tee, old_tee_pad);
      gst_object_unref(old_tee_pad);
      gst_object_unref(source_queue_sink);
      gst_element_unlink(source.fakesink_queue, source.fakesink);
      if (!gst_bin_remove(GST_BIN(source.bin), source.fakesink)) {
        return absl::InternalError(TO_STRING("Could not replace the fake sink for " << channel));
      }
      source.fakesink = nullptr;
      GstElement* ingress_isolation =
          gst_element_factory_make("hmpreviewisolation", (channel + "_gpu_preview_ingress_isolation").c_str());
      GstElement* isolation =
          gst_element_factory_make("hmpreviewisolation", (channel + "_gpu_preview_isolation").c_str());
      GstElement* converter =
          gst_element_factory_make(NVDS_ELEM_VIDEO_CONV, (channel + "_gpu_preview_converter").c_str());
      GstElement* caps_filter =
          gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, (channel + "_gpu_preview_caps").c_str());
      GstElement* sink = gst_element_factory_make("hmgpupreviewsink", (channel + "_gpu_preview_sink").c_str());
      if (!ingress_isolation || !isolation || !converter || !caps_filter || !sink)
        return absl::InternalError(TO_STRING("Could not create the GPU-native preview branch for " << channel));
      gst_bin_add_many(GST_BIN(source.bin), ingress_isolation, isolation, converter, caps_filter, sink, nullptr);
      const NvDsSourceConfig& source_config = app_context->config.multi_source_config[source_index];
      if (!configure_path(
              source.fakesink_queue,
              ingress_isolation,
              isolation,
              converter,
              caps_filter,
              sink,
              channel,
              target->second,
              source_config.gpu_id,
              1280,
              720,
              true)) {
        return absl::InternalError(TO_STRING("Could not link the GPU-native preview branch for " << channel));
      }
      if (!link_element_to_tee_src_pad(source.tee, ingress_isolation)) {
        return absl::InternalError(TO_STRING("Could not attach the GPU-native preview branch for " << channel));
      }
      source.fakesink = sink;
    }

    for (const auto& [channel, window_id] : ui_preview_window_ids_) {
      if (channel.rfind("source", 0) == 0 && !ui_preview_channels_.count(channel)) {
        g_print(
            "HSTREAM_PREVIEW channel=%s status=unavailable generation=%" G_GUINT64_FORMAT
            " message=no active camera source for XID %" G_GUINT64_FORMAT "\n",
            channel.c_str(),
            active_ui_preview_generation_,
            window_id);
      }
    }
    if (!active_ui_preview_channel_.empty() && !ui_preview_channels_.count(active_ui_preview_channel_))
      active_ui_preview_channel_.clear();
    g_print(
        "HSTREAM_PREVIEW_MEMORY image-bytes=%" G_GUINT64_FORMAT " budget-bytes=%" G_GUINT64_FORMAT "\n",
        preview_image_bytes,
        kPreviewImageBudgetBytes);
    return absl::OkStatus();
  }

  if (source_render_window_ids_.empty()) {
    return absl::OkStatus();
  }
  // Legacy, explicitly requested CLI compatibility path. hstream-ui never
  // selects this system-memory renderer; its steady-state previews use the
  // GPU-native branches above. Keep this isolated for older diagnostic tools.
  if (app_contexts.size() != 1) {
    return absl::InvalidArgumentError("--source-render-window-ids requires exactly one active pipeline context");
  }

  constexpr gint kProgramPreviewWidth = 1600;
  constexpr gint kProgramPreviewHeight = 900;
  const auto& app_context = app_contexts.front();
  NvDsSinkBin& output = app_context->pipeline.instance_bins[0].sink_bin;
  GstElement* program_queue = gst_element_factory_make(NVDS_ELEM_QUEUE, "program_preview_queue");
  GstElement* program_converter = gst_element_factory_make(NVDS_ELEM_VIDEO_CONV, "program_preview_converter");
  GstElement* program_caps_filter = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, "program_preview_caps");
  GstElement* program_system_converter = gst_element_factory_make("videoconvert", "program_preview_system_converter");
  GstElement* program_system_caps_filter =
      gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, "program_preview_system_caps");
  GstElement* program_sink = gst_element_factory_make(NVDS_ELEM_SINK_FAKESINK, "program_preview_sink");
  if (!output.bin || !output.tee || !program_queue || !program_converter || !program_caps_filter ||
      !program_system_converter || !program_system_caps_filter || !program_sink) {
    for (GstElement* element : {
             program_queue,
             program_converter,
             program_caps_filter,
             program_system_converter,
             program_system_caps_filter,
             program_sink,
         }) {
      if (element)
        gst_object_unref(element);
    }
    return absl::InternalError("Could not create retained Program preview branch");
  }
  g_object_set(
      G_OBJECT(program_queue),
      "leaky",
      2,
      "max-size-buffers",
      1,
      "max-size-bytes",
      0,
      "max-size-time",
      static_cast<guint64>(0),
      nullptr);
  g_object_set(
      G_OBJECT(program_converter),
      "gpu-id",
      app_context->config.hmsticher_config.gpu_id,
      "nvbuf-memory-type",
      1,
      nullptr);
  GstCaps* program_caps = gst_caps_new_simple(
      "video/x-raw",
      "format",
      G_TYPE_STRING,
      "NV12",
      "width",
      G_TYPE_INT,
      kProgramPreviewWidth,
      "height",
      G_TYPE_INT,
      kProgramPreviewHeight,
      nullptr);
  g_object_set(G_OBJECT(program_caps_filter), "caps", program_caps, nullptr);
  gst_caps_unref(program_caps);
  GstCaps* program_system_caps = gst_caps_new_simple(
      "video/x-raw",
      "format",
      G_TYPE_STRING,
      "BGRx",
      "width",
      G_TYPE_INT,
      kProgramPreviewWidth,
      "height",
      G_TYPE_INT,
      kProgramPreviewHeight,
      nullptr);
  g_object_set(G_OBJECT(program_system_caps_filter), "caps", program_system_caps, nullptr);
  gst_caps_unref(program_system_caps);
  g_object_set(G_OBJECT(program_sink), "sync", FALSE, "async", FALSE, "enable-last-sample", TRUE, nullptr);
  gst_bin_add_many(
      GST_BIN(output.bin),
      program_queue,
      program_converter,
      program_caps_filter,
      program_system_converter,
      program_system_caps_filter,
      program_sink,
      nullptr);
  if (!link_element_to_tee_src_pad(output.tee, program_queue) ||
      !gst_element_link_many(
          program_queue,
          program_converter,
          program_caps_filter,
          program_system_converter,
          program_system_caps_filter,
          program_sink,
          nullptr)) {
    return absl::InternalError("Could not link retained Program preview branch");
  }

  constexpr gint kCameraPreviewWidth = 1280;
  constexpr gint kCameraPreviewHeight = 720;
  for (const auto& app_context : app_contexts) {
    NvDsSrcParentBin& sources = app_context->pipeline.multi_src_bin;
    const guint preview_count = std::min<guint>(sources.num_bins, static_cast<guint>(source_render_window_ids_.size()));
    for (guint source_index = 0; source_index < preview_count; ++source_index) {
      NvDsSrcBin& source = sources.sub_bins[source_index];
      if (!source.bin || !source.fakesink_queue || !source.fakesink) {
        return absl::FailedPreconditionError(
            TO_STRING("Source " << source_index << " does not expose a preview tee branch"));
      }

      gst_element_unlink(source.fakesink_queue, source.fakesink);
      if (!gst_bin_remove(GST_BIN(source.bin), source.fakesink)) {
        return absl::InternalError(TO_STRING("Could not replace source " << source_index << " fake sink"));
      }
      source.fakesink = nullptr;

      const std::string suffix = std::to_string(source_index);
      GstElement* converter =
          gst_element_factory_make(NVDS_ELEM_VIDEO_CONV, ("source_preview_converter_" + suffix).c_str());
      GstElement* caps_filter =
          gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, ("source_preview_caps_" + suffix).c_str());
      GstElement* system_converter =
          gst_element_factory_make("videoconvert", ("source_preview_system_converter_" + suffix).c_str());
      GstElement* system_caps_filter =
          gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, ("source_preview_system_caps_" + suffix).c_str());
      GstElement* sink = gst_element_factory_make("ximagesink", ("source_preview_sink_" + suffix).c_str());
      if (!converter || !caps_filter || !system_converter || !system_caps_filter || !sink ||
          !GST_IS_VIDEO_OVERLAY(sink)) {
        if (converter)
          gst_object_unref(converter);
        if (caps_filter)
          gst_object_unref(caps_filter);
        if (system_converter)
          gst_object_unref(system_converter);
        if (system_caps_filter)
          gst_object_unref(system_caps_filter);
        if (sink)
          gst_object_unref(sink);
        return absl::InternalError(TO_STRING("Could not create embedded preview for source " << source_index));
      }

      const NvDsSourceConfig& source_config = app_context->config.multi_source_config[source_index];
      g_object_set(G_OBJECT(converter), "gpu-id", source_config.gpu_id, "nvbuf-memory-type", 1, nullptr);
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
      // Match the established URI source converter workaround on Jetson.
      g_object_set(G_OBJECT(converter), "copy-hw", 2, nullptr);
#endif

      GstCaps* caps = gst_caps_new_simple(
          "video/x-raw",
          "format",
          G_TYPE_STRING,
          "NV12",
          "width",
          G_TYPE_INT,
          kCameraPreviewWidth,
          "height",
          G_TYPE_INT,
          kCameraPreviewHeight,
          nullptr);
      g_object_set(G_OBJECT(caps_filter), "caps", caps, nullptr);
      gst_caps_unref(caps);
      GstCaps* system_caps = gst_caps_new_simple(
          "video/x-raw",
          "format",
          G_TYPE_STRING,
          "BGRx",
          "width",
          G_TYPE_INT,
          kCameraPreviewWidth,
          "height",
          G_TYPE_INT,
          kCameraPreviewHeight,
          nullptr);
      g_object_set(G_OBJECT(system_caps_filter), "caps", system_caps, nullptr);
      gst_caps_unref(system_caps);
      g_object_set(
          G_OBJECT(source.fakesink_queue),
          "leaky",
          2,
          "max-size-buffers",
          1,
          "max-size-bytes",
          0,
          "max-size-time",
          static_cast<guint64>(0),
          nullptr);
      g_object_set(G_OBJECT(sink), "sync", FALSE, "async", FALSE, "enable-last-sample", TRUE, nullptr);

      gst_bin_add_many(
          GST_BIN(source.bin), converter, caps_filter, system_converter, system_caps_filter, sink, nullptr);
      if (!gst_element_link_many(
              source.fakesink_queue, converter, caps_filter, system_converter, system_caps_filter, sink, nullptr)) {
        return absl::InternalError(TO_STRING("Could not link embedded preview for source " << source_index));
      }
      gst_video_overlay_set_window_handle(
          GST_VIDEO_OVERLAY(sink), static_cast<guintptr>(source_render_window_ids_[source_index]));
      source.fakesink = sink;
      g_print(
          "Using external source %u render window id: %" G_GUINT64_FORMAT "\n",
          source_index,
          source_render_window_ids_[source_index]);
    }
  }
  return absl::OkStatus();
}

/*
struct CameraConnection {
  int sensor_id{0};
  int i2c_bus{0};
  int width{0};
  int height{0};
  int fps_n{0};
  int fps_d{0};
};
*/
absl::Status PipelineApplication::auto_focus_cameras(const std::vector<std::shared_ptr<HmApp>>& app_contexts) const {
  std::vector<hm::camera::CameraConnection> cameras;
  std::set<int> sensors, bus;
  for (const auto& app : app_contexts) {
    for (size_t i = 0; i < app->config.num_source_sub_bins; ++i) {
      const NvDsSourceConfig src_config = app->config.multi_source_config[i];
      if (!src_config.enable) {
        assert(false);
        continue;
      }
      if (src_config.type != NV_DS_SOURCE_CAMERA_CSI) {
        continue;
      }
      // Assert no duplicates
      assert(sensors.emplace(src_config.camera_csi_sensor_id).second);
      assert(bus.emplace(src_config.camera_i2c_bus).second);
      cameras.emplace_back(
          hm::camera::CameraConnection{
              .sensor_id = src_config.camera_csi_sensor_id,
              .i2c_bus = src_config.camera_i2c_bus,
              .width = src_config.camera_width,
              .height = src_config.camera_height,
              .fps_n = src_config.camera_fps_n,
              .fps_d = src_config.camera_fps_d,
          });
    }
  }
  if (cameras.empty()) {
    return absl::OkStatus();
  }
  return hm::camera::auto_focus_cameras(
      cameras, /*show=*/false, /*interactive=*/false, /*verbose=*/false, /*force=*/force_reconfigure_);
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

  const uint64_t main_loop_generation = ++main_loop_generation_;
  cleanup_stack.push([this, main_loop_generation] { cancel_stitch_frame_rewind(main_loop_generation); });

  std::vector<AppCtx*> stage_calibration_contexts;
  for (const auto& context : app_contexts) {
    if (context && context->configurator().stitching_calibration_required()) {
      one_pass_calibration_contexts_.insert(context.get());
      stage_calibration_contexts.push_back(context.get());
    }
  }
  cleanup_stack.push([this, stage_calibration_contexts] {
    for (AppCtx* context : stage_calibration_contexts) {
      one_pass_calibration_contexts_.erase(context);
    }
  });

  _intr_setup();
  const bool calibration_active = std::any_of(app_contexts.begin(), app_contexts.end(), [this](const auto& context) {
    return context && context->configurator().stitching_calibration_required() &&
        stitch_frame_rewound_contexts_.count(context.get()) == 0;
  });
  stitch_frame_calibration_active_.store(calibration_active, std::memory_order_release);
  reset_playback_timing_state(current_stage_);
  g_timeout_add(400, check_for_interrupt_static, nullptr);

  auto owned_windows = std::make_shared<std::set<Window>>();
  cleanup_stack.push([this, contexts = app_contexts, stage = current_stage_, owned_windows] {
    for (const auto& context : contexts) {
      if (!context) {
        continue;
      }
      if (context->return_value == -1) {
        return_value_ = -1;
      }
      destroy_pipeline(context.get());
    }
    const auto stage_windows = stage_windows_.find(stage);
    if (stage_windows != stage_windows_.end()) {
      for (auto& [index, window] : stage_windows->second) {
        (void)index;
        if (window && owned_windows->count(window)) {
          absl::MutexLock lk(&disp_lock_);
          if (display_) {
            XFlush(display_);
            XDestroyWindow(display_, window);
          }
        }
        window = 0;
      }
    }
    if (x_event_thread_ && x_event_thread_->joinable) {
      g_thread_join(x_event_thread_);
      x_event_thread_ = nullptr;
    }
    absl::MutexLock lk(&disp_lock_);
    if (display_) {
      XCloseDisplay(display_);
      display_ = nullptr;
    }
  });

  bool has_video_overlay_sink = false;
  for (const auto& app_ctx : app_contexts) {
    for (guint j = 0; j < app_ctx->config.num_sink_sub_bins; j++) {
      GstElement* sink = app_ctx->pipeline.instance_bins[0].sink_bin.sub_bins[j].sink;
      if (GST_IS_VIDEO_OVERLAY(sink) && !manages_its_own_window(sink)) {
        has_video_overlay_sink = true;
        break;
      }
    }
    if (has_video_overlay_sink) {
      break;
    }
  }

  if (has_video_overlay_sink) {
    display_ = XOpenDisplay(nullptr);
    if (!display_) {
      NVGSTDS_ERR_MSG_V("Could not open X Display");
      return absl::InternalError("Could not open X Display");
    }
  }

  for (guint i = 0; i < app_contexts.size(); i++) {
#if defined(__aarch64__)
    auto pause_status = pause_pipeline_for_model_initialization(app_contexts[i].get(), &cintr_);
    if (!pause_status.ok()) {
      if (absl::IsCancelled(pause_status)) {
        quit_ = TRUE;
        return absl::OkStatus();
      }
      NVGSTDS_ERR_MSG_V("Failed to set pipeline to PAUSED");
      return pause_status;
    }
#endif
    for (guint j = 0; j < app_contexts[i]->config.num_sink_sub_bins; j++) {
      GstElement* sink = app_contexts[i]->pipeline.instance_bins[0].sink_bin.sub_bins[j].sink;
      if (!GST_IS_VIDEO_OVERLAY(sink) || manages_its_own_window(sink))
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
      const bool use_external_window = render_window_id_ > 0 && i == 0;
      if (use_external_window) {
        windows[i] = static_cast<Window>(render_window_id_);
        g_print("Using external render window id: %" G_GINT64_FORMAT "\n", render_window_id_);
      } else {
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
        owned_windows->insert(windows[i]);

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
      }
      gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), (gulong)windows[i]);
      gst_video_overlay_expose(GST_VIDEO_OVERLAY(sink));

      if (!use_external_window && !x_event_thread_)
        x_event_thread_ = g_thread_new("nvds-window-event-thread", nvds_x_event_thread_static, nullptr);
    }
#if !defined(__aarch64__)
    int current_device = -1;
    cudaGetDevice(&current_device);
    struct cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, current_device);
    if (!prop.integrated && !g_getenv("HM_TEST_CONCURRENT_STITCHING_CALIBRATION")) {
      auto pause_status = pause_pipeline_for_model_initialization(app_contexts[i].get(), &cintr_);
      if (!pause_status.ok()) {
        if (absl::IsCancelled(pause_status)) {
          quit_ = TRUE;
          return absl::OkStatus();
        }
        NVGSTDS_ERR_MSG_V("Failed to set pipeline to PAUSED");
        return pause_status;
      }
    }
#endif
  }
  return absl::OkStatus();
}

absl::Status PipelineApplication::stopPipeline(std::shared_ptr<HmApp> app_context) const {
  if (!app_context) {
    return absl::OkStatus();
  }
  GstElement* pipeline = app_context->pipeline.pipeline;
  if (!pipeline) {
    return absl::OkStatus();
  }
  if (one_pass_calibration_contexts_.count(app_context.get()) != 0) {
    // A one-pass generation has no complete downstream stream to finalize
    // until calibration posts its completion message. On interruption, discard
    // that generation directly; waiting for EOS behind a synchronous feature
    // or rink-mask calculation can only time out and there is no valid archive
    // to preserve yet.
    GstElement* stitcher = app_context->pipeline.hmstitcher_bin.elem_hmstitcher;
    if (stitcher) {
      g_object_set(G_OBJECT(stitcher), "cancel-pending-work", TRUE, nullptr);
    }
    app_context->eos_received = TRUE;
    cancel_uri_playlist_frame_barrier(&app_context->pipeline.multi_src_bin);
    if (gst_element_set_state(pipeline, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE) {
      return absl::InternalError("Interrupted calibration pipeline could not be stopped");
    }
    return absl::OkStatus();
  }
  if (!stop_pipeline_gracefully(app_context.get(), 5 * GST_SECOND)) {
    return absl::DeadlineExceededError(
        "Pipeline did not finalize EOS before stopping; encoded output may be incomplete");
  }
  return absl::OkStatus();
}

absl::Status PipelineApplication::playPipelines(
    std::vector<std::shared_ptr<HmApp>>& app_contexts,
    CleanupStack& cleanup_stack) {
  absl::Status status;
  for (guint i = 0; i < app_contexts.size(); i++) {
    const uint64_t initial_position_ns = initial_pipeline_position_ns(app_contexts[i].get());
    status = app_contexts[i]->configurator().post_config_pipeline(
        app_contexts[i]->pipeline, app_contexts[i]->config, initial_position_ns);
    if (!status.ok()) {
      std::cerr << status << std::endl;
      g_print("\npipeline post-configuration failed.\n");
      return absl::InternalError("pipeline post-configuration failed");
    }
    if (gst_element_set_state(app_contexts[i]->pipeline.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      g_print("\ncan't set pipeline to playing state.\n");
      return absl::InternalError("can't set pipeline to playing state");
    }
    emit_ui_startup("frames", "Pipeline is running; waiting for the first processed frame");
    cleanup_stack.push([this, app_ctx = app_contexts[i]]() {
      auto status = stopPipeline(std::move(app_ctx));
      if (!status.ok()) {
        std::cerr << status << std::endl;
      }
    });
    if (dump_pipeline_dot_) {
      hm::save_dot_file(app_contexts[i]->pipeline.pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "pipeline_running");
    }
    if (app_contexts[i]->config.pipeline_recreate_sec)
      g_timeout_add_seconds(
          app_contexts[i]->config.pipeline_recreate_sec, recreate_pipeline_thread_func_static, app_contexts[i].get());
  }

  print_runtime_commands();
  changemode(1);
  g_timeout_add(40, event_thread_func_static, nullptr);
  if (!ui_preview_channels_.empty()) {
    // Re-arm the initial channel directly after PLAYING. Startup must not race
    // an external command against installation of the GLib stdin poll.
    const std::string channel =
        active_ui_preview_channel_.empty() ? initial_ui_preview_channel_ : active_ui_preview_channel_;
    const guint64 generation = active_ui_preview_generation_ + 1;
    g_print(
        "HSTREAM_PREVIEW_RUNTIME status=ready channel=%s generation=%" G_GUINT64_FORMAT "\n",
        channel.c_str(),
        generation);
    set_preview_active_runtime(channel, generation);
  }
  g_main_loop_run(main_loop_);
  changemode(0);

  // Finalize muxers before reporting success or allowing cleanup to set the
  // pipeline to NULL. In particular, a UI Stop/SIGINT must leave a playable
  // archive rather than a truncated container that merely came from exit 0.
  for (const std::shared_ptr<HmApp>& app_context : app_contexts) {
    const absl::Status stop_status = stopPipeline(app_context);
    if (!stop_status.ok()) {
      std::cerr << stop_status << std::endl;
      return_value_ = -1;
    }
  }

  // Bus errors are recorded on their individual AppCtx instances. Cleanup used
  // to copy those values into return_value_, but cleanup runs only after this
  // method returns, so a failed pipeline was announced as successful and the
  // process exited zero. Aggregate the results while every context is still
  // alive and before deciding the run status.
  for (const std::shared_ptr<HmApp>& app_context : app_contexts) {
    if (app_context && app_context->return_value != 0) {
      return_value_ = -1;
    }
  }

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

template <typename E_TYPE>
absl::StatusOr<std::vector<std::set<E_TYPE>>> parse_types(
    const std::string type_name,
    char** enable_args,
    const std::function<std::optional<E_TYPE>(const std::string&)>& type_from_string_fn) {
  if (!enable_args || !*enable_args) {
    return std::vector<std::set<E_TYPE>>{};
  }
  auto trim_ascii = [](std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c); };
    s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), is_space));
    s.erase(std::find_if_not(s.rbegin(), s.rend(), is_space).base(), s.end());
    return s;
  };
  std::vector<std::set<E_TYPE>> stage_enabled_types;
  for (size_t i = 0, n = g_strv_length(enable_args); i < n; ++i) {
    // Individual items can split by a comma
    std::vector<std::string> p_each = absl::StrSplit(enable_args[i], ',');
    stage_enabled_types.emplace_back();
    for (const std::string& stype : p_each) {
      const std::string token = trim_ascii(stype);
      if (token.empty()) {
        continue;
      }
      if (std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c); })) {
        E_TYPE type = static_cast<E_TYPE>(std::stoi(token.c_str()));
        if (!type) {
          return absl::InvalidArgumentError(TO_STRING("Invalid " << type_name << " type " << token));
        }
        stage_enabled_types.rbegin()->emplace(type);
      } else {
        auto type_enum = type_from_string_fn(token);
        if (!type_enum) {
          return absl::InvalidArgumentError(TO_STRING("Invalid " << type_name << " type " << token));
        }
        stage_enabled_types.rbegin()->emplace(*type_enum);
      }
    }
  }
  return stage_enabled_types;
}

//------------------------------------------------------------------------------
// Main run function.
//------------------------------------------------------------------------------
absl::Status PipelineApplication::run(int argc, char* argv[]) {
  absl::Status status = absl::OkStatus();
  GError* error = nullptr;
  char* start_time{nullptr};
  char* stitch_frame_time{nullptr};
  std::vector<std::string> normalized_args = normalize_cli_args(argc, argv);
  std::vector<char*> normalized_argv = make_mutable_argv(normalized_args);
  int normalized_argc = static_cast<int>(normalized_argv.size());

  CleanupStack global_cleanup_stack;
  char** pipline_options{nullptr};
  GOptionEntry entries[] = {
      {"version", 'v', 0, G_OPTION_ARG_NONE, &print_version_, "Print DeepStreamSDK version", nullptr},
      {"show", 0, 0, G_OPTION_ARG_NONE, &show_, "Enable render sink output", nullptr},
      {"show-stitching",
       0,
       0,
       G_OPTION_ARG_DOUBLE,
       &show_stitching_scale_,
       "Show hmstitcher display output (`0` disables, `N` scales render window)",
       "RATIO"},
      {"show-playtracker",
       0,
       0,
       G_OPTION_ARG_DOUBLE,
       &show_playtracker_scale_,
       "Show ds-playtracker display output (`0` disables, `N` scales render window)",
       "RATIO"},
      {"show-scaled",
       0,
       0,
       G_OPTION_ARG_DOUBLE,
       &show_scaled_scale_,
       "Scale final render window for --show (`0` disables, `N` is scale ratio)",
       "RATIO"},
      {"render-window-id",
       0,
       0,
       G_OPTION_ARG_INT64,
       &render_window_id_,
       "Native X11 window id to use for the render sink instead of creating a DeepStream window",
       "XID"},
      {"headless-render-video",
       0,
       0,
       G_OPTION_ARG_NONE,
       &headless_render_video_,
       "Replace render-type video output with an unsynchronized fakesink while retaining render audio routing",
       nullptr},
      {"ui-preview-windows",
       0,
       0,
       G_OPTION_ARG_CALLBACK,
       (gpointer) + [](const gchar*, const gchar* value, gpointer data, GError** error) -> gboolean {
         auto* app = static_cast<PipelineApplication*>(data);
         std::map<std::string, guint64> parsed;
         std::set<guint64> window_ids;
         for (absl::string_view part : absl::StrSplit(value ? value : "", ',')) {
           const size_t separator = part.find(':');
           if (separator == absl::string_view::npos) {
             g_set_error(
                 error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Invalid --ui-preview-windows entry: %s", value);
             return FALSE;
           }
           std::string channel(part.substr(0, separator));
           std::string id_text(part.substr(separator + 1));
           channel.erase(
               std::remove_if(channel.begin(), channel.end(), [](unsigned char c) { return std::isspace(c); }),
               channel.end());
           id_text.erase(
               std::remove_if(id_text.begin(), id_text.end(), [](unsigned char c) { return std::isspace(c); }),
               id_text.end());
           const bool valid_channel = channel == "program" || channel == "stitched" ||
               (channel.rfind("source", 0) == 0 && channel.size() > 6 &&
                std::all_of(channel.begin() + 6, channel.end(), [](unsigned char c) { return std::isdigit(c); }));
           if (!valid_channel || id_text.empty() ||
               !std::all_of(id_text.begin(), id_text.end(), [](unsigned char c) { return std::isdigit(c); })) {
             g_set_error(
                 error,
                 G_OPTION_ERROR,
                 G_OPTION_ERROR_BAD_VALUE,
                 "Invalid --ui-preview-windows entry: %s",
                 std::string(part).c_str());
             return FALSE;
           }
           errno = 0;
           gchar* end = nullptr;
           const guint64 window_id = g_ascii_strtoull(id_text.c_str(), &end, 10);
           if (errno == ERANGE || window_id == 0 || !end || *end != '\0' || parsed.count(channel) ||
               window_ids.count(window_id)) {
             g_set_error(
                 error,
                 G_OPTION_ERROR,
                 G_OPTION_ERROR_BAD_VALUE,
                 "Duplicate, zero, or invalid UI preview target: %s",
                 std::string(part).c_str());
             return FALSE;
           }
           parsed.emplace(channel, window_id);
           window_ids.insert(window_id);
         }
         if (parsed.empty()) {
           g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "--ui-preview-windows cannot be empty");
           return FALSE;
         }
         app->ui_preview_window_ids_ = std::move(parsed);
         return TRUE;
       },
       "GPU-native UI preview targets as logical-channel:XID pairs",
       "program:XID,stitched:XID,source0:XID,..."},
      {"ui-preview-active",
       0,
       0,
       G_OPTION_ARG_CALLBACK,
       (gpointer) + [](const gchar*, const gchar* value, gpointer data, GError** error) -> gboolean {
         auto* app = static_cast<PipelineApplication*>(data);
         const std::string channel = value ? value : "";
         const bool valid = channel == "none" || channel == "program" || channel == "stitched" ||
             (channel.rfind("source", 0) == 0 && channel.size() > 6 &&
              std::all_of(channel.begin() + 6, channel.end(), [](unsigned char c) { return std::isdigit(c); }));
         if (!valid) {
           g_set_error(
               error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Invalid --ui-preview-active channel: %s", value);
           return FALSE;
         }
         app->initial_ui_preview_channel_ = channel;
         return TRUE;
       },
       "Initially active GPU-native UI preview channel",
       "CHANNEL"},
      {"source-render-window-ids",
       0,
       0,
       G_OPTION_ARG_CALLBACK,
       (gpointer) + [](const gchar*, const gchar* value, gpointer data, GError** error) -> gboolean {
         auto* app = static_cast<PipelineApplication*>(data);
         std::vector<guint64> parsed;
         for (absl::string_view part : absl::StrSplit(value ? value : "", ',')) {
           std::string token(part);
           token.erase(
               std::remove_if(token.begin(), token.end(), [](unsigned char c) { return std::isspace(c); }),
               token.end());
           if (token.empty()) {
             g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Empty XID in --source-render-window-ids");
             return FALSE;
           }
           if (!std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c); })) {
             g_set_error(
                 error,
                 G_OPTION_ERROR,
                 G_OPTION_ERROR_BAD_VALUE,
                 "Invalid XID in --source-render-window-ids: %s",
                 token.c_str());
             return FALSE;
           }
           errno = 0;
           gchar* end = nullptr;
           const guint64 window_id = g_ascii_strtoull(token.c_str(), &end, 10);
           if (errno == ERANGE || window_id == 0 || !end || *end != '\0') {
             g_set_error(
                 error,
                 G_OPTION_ERROR,
                 G_OPTION_ERROR_BAD_VALUE,
                 "Invalid XID in --source-render-window-ids: %s",
                 token.c_str());
             return FALSE;
           }
           parsed.push_back(window_id);
         }
         app->source_render_window_ids_ = std::move(parsed);
         return TRUE;
       },
       "Comma-separated native X11 window ids for source camera previews",
       "XID,..."},
      {"stitch-rotate-degrees",
       0,
       0,
       G_OPTION_ARG_CALLBACK,
       (gpointer) + [](const gchar*, const gchar* value, gpointer data, GError** error) -> gboolean {
         auto* app = static_cast<PipelineApplication*>(data);
         double degrees = 0.0;
         if (!parse_finite_double(value, degrees)) {
           g_set_error(
               error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Invalid --stitch-rotate-degrees value: %s", value);
           return FALSE;
         }
         app->stitch_rotate_degrees_ = degrees;
         app->stitch_rotate_degrees_set_ = TRUE;
         return TRUE;
       },
       "Rotate stitched panorama about its center after stitching",
       "DEGREES"},
      {"tiledtext", 0, 0, G_OPTION_ARG_NONE, &show_bbox_text_, "Display Bounding box labels in tiled mode", nullptr},
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
      {"gpu-id", 'x', 0, G_OPTION_ARG_INT, &override_gpu_id_, "Set the GPU id to use", nullptr},
      {"time-limit",
       't',
       0,
       G_OPTION_ARG_INT,
       &time_limit_seconds_,
       "Stop after processing this many seconds of video",
       "N"},
      {"progress-ui",
       0,
       0,
       G_OPTION_ARG_NONE,
       &progress_ui_enabled_,
       "Enable HM-style full-screen terminal progress UI with status table, progress bar, log window, and graph",
       nullptr},
      {"progress-bar-lines",
       0,
       0,
       G_OPTION_ARG_INT,
       &progress_ui_lines_,
       "Number of log lines to show in --progress-ui",
       "N"},
      {"progress-ui-lines", 0, 0, G_OPTION_ARG_INT, &progress_ui_lines_, "Alias for --progress-bar-lines", "N"},
      {"progress-ui-refresh-ms",
       0,
       0,
       G_OPTION_ARG_INT,
       &progress_ui_refresh_ms_,
       "Minimum terminal refresh interval for --progress-ui",
       "MS"},
      {"progress-ui-start-threshold",
       0,
       0,
       G_OPTION_ARG_INT,
       &progress_ui_start_threshold_,
       "Perf updates to wait before drawing --progress-ui",
       "N"},
      {"progress-ui-graph",
       0,
       0,
       G_OPTION_ARG_NONE,
       &progress_ui_graph_,
       "Show the pipeline graph panel in --progress-ui",
       nullptr},
      {"progress-ui-no-graph",
       0,
       0,
       G_OPTION_ARG_NONE,
       &progress_ui_no_graph_,
       "Hide the pipeline graph panel in --progress-ui",
       nullptr},
      {"progress-ui-no-capture",
       0,
       0,
       G_OPTION_ARG_NONE,
       &progress_ui_no_capture_,
       "Do not capture terminal output into the --progress-ui log window",
       nullptr},
      {"options", 'p', 0, G_OPTION_ARG_FILENAME_ARRAY, &pipline_options, "Set arbitrary option(s)", nullptr},
      {"cfg-file", 'c', 0, G_OPTION_ARG_FILENAME_ARRAY, &cfg_files_, "Set the config file", "FILE"},
      {"enable-sources", 'e', 0, G_OPTION_ARG_FILENAME_ARRAY, &enable_sources_, "Enable Sources", nullptr},
      {"enable-sinks",
       'k',
       0,
       G_OPTION_ARG_FILENAME_ARRAY,
       &enable_sinks_,
       "Enable sinks: FAKE=discard, RENDER=display, ENCODE_FILE=write video, RTSP/UDPSINK/RTMP=server sink "
       "(RTSP unless output-file starts with rtmp://; aliases enable the same sink type), WEBRTC=browser preview, "
       "RENDER_DRM=DRM display, MSG_CONV_BROKER=message broker",
       "SINK[,SINK...]"},
      {"game-id", 'g', 0, G_OPTION_ARG_FILENAME_ARRAY, &game_id_, "Game ID", nullptr},
      {"force-reconfigure", 'f', 0, G_OPTION_ARG_NONE, &force_reconfigure_, "Force reconfigure", nullptr},
      {"clean", 0, 0, G_OPTION_ARG_NONE, &clean_stitching_artifacts_, "Clean stitching artifacts and exit", nullptr},
      {"clean-from-control-points",
       0,
       0,
       G_OPTION_ARG_NONE,
       &clean_stitching_from_control_points_,
       "Clean control-point-dependent stitching artifacts and exit",
       nullptr},
      {"clean-expected-invalidation-id",
       0,
       0,
       G_OPTION_ARG_STRING,
       &clean_stitching_expected_invalidation_id_,
       "Apply stitching changes only if this invalidation is still current",
       "ID"},
      {"start-time", 's', 0, G_OPTION_ARG_STRING, &start_time, "Start time", nullptr},
      {"stitch-frame-time",
       0,
       0,
       G_OPTION_ARG_STRING,
       &stitch_frame_time,
       "Use the frame at this timestamp for one-pass stitching calibration",
       "HH:MM:SS"},
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
  GOptionGroup* group = g_option_group_new("abc", nullptr, nullptr, this, nullptr);
  g_option_group_add_entries(group, entries);
  g_option_context_set_main_group(ctx, group);
  g_option_context_add_group(ctx, gst_init_get_option_group());

  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);

  char** normalized_argv_data = normalized_argv.data();
  if (!g_option_context_parse(ctx, &normalized_argc, &normalized_argv_data, &error)) {
    NVGSTDS_ERR_MSG_V("%s", error->message);
    return absl::InternalError(error->message);
  }

  if (!ui_preview_window_ids_.empty()) {
    if (!hm::gpu_preview::renderer_available() || !hm::gpu_preview::register_elements()) {
      return absl::FailedPreconditionError(
          "GPU-native embedded preview is unavailable on this platform; no CPU preview fallback was enabled");
    }
    if (initial_ui_preview_channel_ != "none" && !ui_preview_window_ids_.count(initial_ui_preview_channel_)) {
      return absl::InvalidArgumentError("--ui-preview-active must name a channel present in --ui-preview-windows");
    }
    active_ui_preview_channel_ = initial_ui_preview_channel_ == "none" ? std::string() : initial_ui_preview_channel_;
    active_ui_preview_generation_ = 1;
  }
  set_embedded_gpu_preview_video_mode(headless_render_video_ || !ui_preview_window_ids_.empty());

  constexpr const char* kCalibrationInvalidationEnvironment = "HSTREAM_CALIBRATION_INVALIDATION_ID";
  if (clean_stitching_expected_invalidation_id_ != nullptr) {
    if (*clean_stitching_expected_invalidation_id_ == '\0') {
      return absl::InvalidArgumentError("--clean-expected-invalidation-id requires a non-empty ID");
    }
    const char* runtime_invalidation_id = g_getenv(kCalibrationInvalidationEnvironment);
    if (runtime_invalidation_id != nullptr && *runtime_invalidation_id != '\0' &&
        g_strcmp0(runtime_invalidation_id, clean_stitching_expected_invalidation_id_) != 0) {
      return absl::InvalidArgumentError(
          "--clean-expected-invalidation-id conflicts with HSTREAM_CALIBRATION_INVALIDATION_ID");
    }
    if (!g_setenv(kCalibrationInvalidationEnvironment, clean_stitching_expected_invalidation_id_, /*overwrite=*/TRUE)) {
      return absl::InternalError("Unable to publish the stitching invalidation ID to runtime plugins");
    }
  } else {
    // The parsed CLI value is the single source of truth. Do not let an
    // inherited runtime-only token fence plugins differently from Configurator.
    g_unsetenv(kCalibrationInvalidationEnvironment);
  }

#ifndef IS_TEGRA
  if (render_window_id_ > 0) {
    const char* configured_sink = std::getenv("HM_RENDER_SINK");
    if (configured_sink != nullptr && *configured_sink != '\0' &&
        g_ascii_strcasecmp(configured_sink, "ximagesink") != 0 && g_ascii_strcasecmp(configured_sink, "ximage") != 0) {
      g_printerr(
          "Ignoring HM_RENDER_SINK=%s because --render-window-id requires the stable ximagesink X11 embedding "
          "path\n",
          configured_sink);
    }
    ::setenv("HM_RENDER_SINK", "ximagesink", 1);
  }
#endif

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

  if (start_time) {
    const auto parsed_start_time = parse_time_option("--start-time", start_time);
    g_free(start_time);
    if (!parsed_start_time.ok()) {
      if (stitch_frame_time)
        g_free(stitch_frame_time);
      return parsed_start_time.status();
    }
    start_time_ns_ = *parsed_start_time;
  }
  if (stitch_frame_time) {
    const auto parsed_stitch_frame_time = parse_stitch_frame_time_option("--stitch-frame-time", stitch_frame_time);
    g_free(stitch_frame_time);
    if (!parsed_stitch_frame_time.ok()) {
      return parsed_stitch_frame_time.status();
    }
    stitch_frame_time_ns_ = *parsed_stitch_frame_time;
    stitch_frame_time_override_config_value_ = normalized_stitch_frame_time_config_value(stitch_frame_time_ns_);
    stitch_frame_time_set_ = true;
  }

  if (progress_ui_enabled_) {
    hm::TerminalProgressOptions progress_options;
    progress_options.log_lines = progress_ui_lines_;
    progress_options.refresh_ms = progress_ui_refresh_ms_;
    progress_options.start_threshold = progress_ui_start_threshold_;
    progress_options.show_graph = progress_ui_graph_ && !progress_ui_no_graph_;
    progress_options.capture_output = !progress_ui_no_capture_;
    progress_ui_ = std::make_unique<hm::TerminalProgressUi>(progress_options);
    if (progress_ui_->start()) {
      hm::TerminalProgressSnapshot initial_snapshot;
      initial_snapshot.title = game_id_ && *game_id_ ? *game_id_ : "hstream";
      initial_snapshot.stats.push_back({"Status", "starting"});
      initial_snapshot.stats.push_back({"Stage", "configuration"});
      initial_snapshot.completed_text = "00:00:00";
      initial_snapshot.total_text = "--:--:--";
      progress_ui_->update(std::move(initial_snapshot));
      global_cleanup_stack.push([this] { progress_ui_.reset(); });
    } else {
      progress_ui_.reset();
      g_printerr("--progress-ui requested, but stderr is not an interactive terminal; using regular output\n");
    }
  }

  if (input_uris_) {
    num_input_uris_ = g_strv_length(input_uris_);
  }
  if (!cfg_files_ || g_strv_length(cfg_files_) == 0) {
    cfg_files_ = g_new0(gchar*, 2);
    cfg_files_[0] = g_strdup(default_config_file_name_);
    global_cleanup_stack.push([this] {
      g_strfreev(cfg_files_);
      cfg_files_ = nullptr;
    });
  }

  // Cleaning only removes generated game artifacts and must remain usable
  // offline, even when declared models are not installed.
  if (!clean_stitching_artifacts_ && !clean_stitching_from_control_points_) {
    emit_ui_startup("assets", "Checking pretrained assets");
    std::vector<fs::path> asset_configs;
    for (size_t index = 0, count = g_strv_length(cfg_files_); index < count; ++index)
      asset_configs.emplace_back(cfg_files_[index]);
    HM_RETURN_IF_ERROR(hm::assets::AssetManager::Ensure(asset_configs));
  }

  if (pipline_options) {
    pipeline_options_.clear();
    for (size_t i = 0, n = g_strv_length(pipline_options); i < n; ++i) {
      pipeline_options_.emplace_back();
      // Individual items can split by a comma
      std::vector<std::string> p_each = absl::StrSplit(pipline_options[i], ',');
      for (const std::string& opt : p_each) {
        std::vector<std::string> kv = absl::StrSplit(opt, '=');
        if (kv.size() != 2) {
          return absl::InvalidArgumentError(
              TO_STRING("Pipeline options should use key/value pairs, but got: \"" << opt << "\""));
        }
        pipeline_options_[i].emplace(kv.at(0), kv.at(1));
      }
    }
  }
  std::map<std::string, std::string> show_options;
  if (show_stitching_scale_ >= 0) {
    show_options.emplace("pipeline.hmstitcher.show", show_stitching_scale_ == 0.0 ? "0" : "1");
  }
  if (show_playtracker_scale_ >= 0) {
    show_options.emplace("pipeline.ds-playtracker.show", show_playtracker_scale_ == 0.0 ? "0" : "1");
  }
  if (!show_options.empty()) {
    pipeline_options_.push_back(std::move(show_options));
  }
  if (stitch_rotate_degrees_set_) {
    const std::string degrees = std::to_string(stitch_rotate_degrees_);
    pipeline_options_.push_back({
        {"stitching.post_stitch_rotate_degrees", degrees},
        {"pipeline.hmstitcher.post-stitch-rotate-degrees", degrees},
    });
  }

  gdouble render_scale = -1.0;
  const bool show_stitching_set = show_stitching_scale_ >= 0;
  const bool show_playtracker_set = show_playtracker_scale_ >= 0;
  if (show_scaled_scale_ >= 0) {
    render_scale = show_scaled_scale_;
  } else if (show_stitching_scale_ > 0) {
    render_scale = show_stitching_scale_;
  } else if (show_playtracker_scale_ > 0) {
    render_scale = show_playtracker_scale_;
  } else if (show_stitching_set && show_playtracker_set) {
    // Both plugin-specific selectors were explicitly provided but did not enable any output.
    render_scale = 0.0;
  }
  show_render_scale_ = render_scale;
  show_ = show_ || show_scaled_scale_ > 0 || show_stitching_scale_ > 0 || show_playtracker_scale_ > 0;
  if (show_scaled_scale_ == 0.0) {
    show_ = FALSE;
  }

  HM_ASSIGN_OR_RETURN(
      enabled_source_types_, parse_types<NvDsSourceType>("source", enable_sources_, hm::source_type_from_string));

  HM_ASSIGN_OR_RETURN(enabled_sink_types_, parse_types<NvDsSinkType>("sink", enable_sinks_, hm::sink_type_from_string));

  emit_ui_startup("configuration", "Loading game configuration and saved Left/Right video assignments");
  HM_RETURN_IF_ERROR(initializeInstances(global_cleanup_stack));

  size_t stage_count = 0;
  for (auto stage_item : stage_app_contexts_) {
    current_stage_ = stage_item.first;
    auto& app_contexts = stage_app_contexts_.at(current_stage_);
    {
      auto cache_lock_cleanup = absl::MakeCleanup([] { hm::pipeline::ReleaseTensorRtModelCacheLocks(); });
      emit_ui_startup("stitching", "Discovering source chapters and validating saved stitching artifacts");
      HM_RETURN_IF_ERROR(configureInstances(stage_count, app_contexts));
      if (!app_contexts.empty()) {
        CleanupStack stage_cleanup_stack;
        emit_ui_startup("pipeline", "Creating the GPU pipeline and loading plugins");
        HM_RETURN_IF_ERROR(createPipelines(app_contexts, stage_cleanup_stack));
        emit_ui_startup("video", "Opening video sources and preparing output branches");
        HM_RETURN_IF_ERROR(createMainLoop(app_contexts, stage_windows_[current_stage_], stage_cleanup_stack));
        if (quit_) {
          return absl::OkStatus();
        }
        hm::pipeline::ReleaseTensorRtModelCacheLocks();
        // editor_thread_ = hm::edit_pipeline(GST_OBJECT(app_contexts[0]->pipeline.pipeline));
        emit_ui_startup("decoding", "Starting decoders and waiting for the first frame");
        HM_RETURN_IF_ERROR(playPipelines(app_contexts, stage_cleanup_stack));
      }
      HM_RETURN_IF_ERROR(waitForPipelinesStopped(app_contexts));
    }
    if (stage_count) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    ++stage_count;
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
  if (progress_ui_) {
    progress_ui_->restoreTerminalForInterrupt();
  }
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

void PipelineApplication::reset_playback_timing_state(long stage) {
  g_mutex_lock(&fps_lock_);
  for (auto& [index, state] : progress_states_) {
    (void)index;
    state.initialized = false;
    state.total_video_ns = GST_CLOCK_TIME_NONE;
    state.rate_estimator.reset();
  }
  ui_progress_by_stage_.erase(stage);
  g_mutex_unlock(&fps_lock_);
  std::lock_guard<std::mutex> lock(playback_timing_mu_);
  have_first_pts_ = false;
  first_pts_ns_ = 0;
  have_first_frame_by_source_.fill(false);
  first_frame_numbers_by_source_.fill(0);
  timed_run_last_progress_ns_ = GST_CLOCK_TIME_NONE;
  timed_run_last_progress_wall_ = time_limit_seconds_ > 0 &&
          hm::pipeline_internal::stitch_frame_should_account_playback(
                                      stitch_frame_calibration_active_.load(std::memory_order_acquire))
      ? std::chrono::steady_clock::now()
      : std::chrono::steady_clock::time_point{};
}

void PipelineApplication::record_timed_run_progress(uint64_t processed_ns) {
  if (time_limit_seconds_ <= 0 || processed_ns == GST_CLOCK_TIME_NONE ||
      !hm::pipeline_internal::stitch_frame_should_account_playback(
          stitch_frame_calibration_active_.load(std::memory_order_acquire))) {
    return;
  }
  std::lock_guard<std::mutex> lock(playback_timing_mu_);
  if (timed_run_last_progress_ns_ == GST_CLOCK_TIME_NONE || processed_ns > timed_run_last_progress_ns_) {
    timed_run_last_progress_ns_ = processed_ns;
    timed_run_last_progress_wall_ = std::chrono::steady_clock::now();
  }
}

hm::PlaybackProgressMetrics PipelineApplication::collect_progress_metrics(AppCtx* app_ctx) {
  hm::PlaybackProgressMetrics metrics;
  if (!app_ctx || !app_ctx->pipeline.pipeline ||
      !hm::pipeline_internal::stitch_frame_should_account_playback(
          stitch_frame_calibration_active_.load(std::memory_order_acquire))) {
    return metrics;
  }

  ProgressState& state = progress_states_[app_ctx->index];
  if (!state.initialized) {
    std::vector<uint64_t> source_durations;
    source_durations.reserve(app_ctx->config.num_source_sub_bins);
    guint enabled_uri_sources = 0;
    const bool stitched_output = app_ctx->config.hmsticher_config.enable;
    for (guint i = 0; i < app_ctx->config.num_source_sub_bins; ++i) {
      const NvDsSourceConfig& source_config = app_ctx->config.multi_source_config[i];
      if (!source_config.enable ||
          (source_config.type != NV_DS_SOURCE_URI && source_config.type != NV_DS_SOURCE_URI_MULTIPLE)) {
        continue;
      }
      enabled_uri_sources++;
      uint64_t source_duration_ns = duration_for_source_ns(source_config);
      if (source_duration_ns != GST_CLOCK_TIME_NONE) {
        if (stitched_output) {
          source_duration_ns = subtract_duration_ns(
              source_duration_ns, hmstitcher_source_offset_ns(app_ctx->config.hmsticher_config, i));
        }
        source_durations.emplace_back(source_duration_ns);
      }
    }

    const bool have_complete_stitched_source_durations =
        stitched_output && enabled_uri_sources > 0 && source_durations.size() == enabled_uri_sources;
    if (!source_durations.empty() && (!stitched_output || have_complete_stitched_source_durations)) {
      if (stitched_output && source_durations.size() > 1) {
        state.total_video_ns = *std::min_element(source_durations.begin(), source_durations.end());
      } else {
        state.total_video_ns = *std::max_element(source_durations.begin(), source_durations.end());
      }
    } else {
      gint64 queried_duration = 0;
      if (gst_element_query_duration(app_ctx->pipeline.pipeline, GST_FORMAT_TIME, &queried_duration) &&
          queried_duration > 0) {
        state.total_video_ns = static_cast<uint64_t>(queried_duration);
      }
    }

    if (state.total_video_ns != GST_CLOCK_TIME_NONE && start_time_ns_ > 0) {
      state.total_video_ns = state.total_video_ns > start_time_ns_ ? state.total_video_ns - start_time_ns_ : 0;
    }
    if (time_limit_seconds_ > 0) {
      const uint64_t limit_ns = static_cast<uint64_t>(time_limit_seconds_) * GST_SECOND;
      state.total_video_ns =
          state.total_video_ns == GST_CLOCK_TIME_NONE ? limit_ns : std::min(state.total_video_ns, limit_ns);
    }
    state.initialized = true;
  }

  gint64 queried_position = 0;
  uint64_t processed_ns = GST_CLOCK_TIME_NONE;
  if (gst_element_query_position(app_ctx->pipeline.pipeline, GST_FORMAT_TIME, &queried_position) &&
      queried_position >= 0) {
    processed_ns = static_cast<uint64_t>(queried_position);
    if (start_time_ns_ > 0 && processed_ns >= start_time_ns_) {
      processed_ns -= start_time_ns_;
    }
  }

  if (processed_ns == GST_CLOCK_TIME_NONE) {
    return metrics;
  }
  record_timed_run_progress(processed_ns);
  if (time_limit_seconds_ > 0) {
    const uint64_t limit_ns = static_cast<uint64_t>(time_limit_seconds_) * GST_SECOND;
    if (processed_ns >= limit_ns && !quit_) {
      quit_ = TRUE;
      if (main_loop_) {
        g_main_loop_quit(main_loop_);
      }
    }
  }
  if (state.total_video_ns != GST_CLOCK_TIME_NONE) {
    processed_ns = std::min(processed_ns, state.total_video_ns);
  }

  const auto now = std::chrono::steady_clock::now();
  uint64_t remaining_video_ns = GST_CLOCK_TIME_NONE;
  double fraction = 0.0;
  if (state.total_video_ns != GST_CLOCK_TIME_NONE) {
    remaining_video_ns = state.total_video_ns > processed_ns ? state.total_video_ns - processed_ns : 0;
    if (state.total_video_ns > 0) {
      fraction = static_cast<double>(processed_ns) / static_cast<double>(state.total_video_ns);
    }
  }
  const auto rate = state.rate_estimator.sample(
      processed_ns,
      remaining_video_ns,
      now,
      std::chrono::seconds(std::max(1U, app_ctx->config.perf_measurement_interval_sec)));

  metrics.valid = true;
  metrics.processed_ns = processed_ns;
  metrics.total_ns = state.total_video_ns;
  metrics.remaining_ns = remaining_video_ns;
  metrics.eta_ns = rate.eta_ns;
  metrics.speed_x = rate.speed_x;
  metrics.fraction = fraction;
  return metrics;
}

std::string PipelineApplication::format_progress_status(const hm::PlaybackProgressMetrics& metrics) const {
  if (!metrics.valid) {
    return "";
  }
  std::ostringstream out;
  out << " | Video " << format_duration_ns(metrics.processed_ns) << "/" << format_duration_ns(metrics.total_ns)
      << " | Left " << format_duration_ns(metrics.remaining_ns) << " | ETA " << format_duration_ns(metrics.eta_ns);
  if (metrics.speed_x > 0.0) {
    out << " | " << std::fixed << std::setprecision(2) << metrics.speed_x << "x";
  } else {
    out << " | warming up";
  }
  if (metrics.total_ns != GST_CLOCK_TIME_NONE) {
    out << " | " << format_progress_bar(metrics.fraction) << " " << std::fixed << std::setprecision(1)
        << (std::clamp(metrics.fraction, 0.0, 1.0) * 100.0) << "%";
  }
  return out.str();
}

hm::TerminalProgressSnapshot PipelineApplication::make_terminal_progress_snapshot(
    AppCtx* app_ctx,
    NvDsAppPerfStruct* str,
    const hm::PlaybackProgressMetrics& metrics) const {
  hm::TerminalProgressSnapshot snapshot;
  snapshot.title = game_id_ && *game_id_ ? *game_id_ : "hstream";
  if (current_stage_ != 0) {
    snapshot.title += " stage ";
    snapshot.title += std::to_string(current_stage_);
  }

  const guint numf = str ? str->num_instances : 0;
  for (guint i = 0; i < numf; ++i) {
    std::string label = (str->aggregate_output_fps && numf == 1) ? "Output FPS" : ("FPS " + std::to_string(i));
    snapshot.stats.push_back({std::move(label), format_fixed(fps_[i]) + " (" + format_fixed(fps_avg_[i]) + ")"});
  }
  snapshot.stats.push_back({"Dataset length", format_duration_ns(metrics.total_ns)});
  snapshot.stats.push_back({"Processed", format_duration_ns(metrics.processed_ns)});
  snapshot.stats.push_back({"Remaining", format_duration_ns(metrics.remaining_ns)});
  snapshot.stats.push_back({"ETA", format_duration_ns(metrics.eta_ns)});
  snapshot.stats.push_back({"Speed", metrics.speed_x > 0.0 ? format_fixed(metrics.speed_x) + "x" : "warming up"});
  snapshot.stats.push_back({"Stage", std::to_string(current_stage_)});
  if (app_ctx) {
    snapshot.stats.push_back({"Sources", std::to_string(app_ctx->config.num_source_sub_bins)});
    snapshot.stats.push_back({"Sinks", std::to_string(app_ctx->config.num_sink_sub_bins)});
    if (app_ctx->config.hmsticher_config.enable) {
      snapshot.stats.push_back({"Stitching", "ENABLED"});
    }
    guint audio_bins = 0;
    for (guint i = 0; i < MAX_SOURCE_BINS; ++i) {
      if (app_ctx->config.hmaudio_config[i].enable) {
        ++audio_bins;
      }
    }
    if (audio_bins > 0) {
      snapshot.stats.push_back({"Audio", std::to_string(audio_bins) + " bin" + (audio_bins == 1 ? "" : "s")});
    }
  }

  snapshot.completed_text = format_duration_ns(metrics.processed_ns);
  snapshot.total_text = format_duration_ns(metrics.total_ns);
  if (metrics.processed_ns != GST_CLOCK_TIME_NONE) {
    snapshot.completed = metrics.processed_ns / GST_SECOND;
  }
  if (metrics.total_ns != GST_CLOCK_TIME_NONE) {
    snapshot.total = std::max<uint64_t>(1, metrics.total_ns / GST_SECOND);
  }
  snapshot.complete = metrics.total_ns != GST_CLOCK_TIME_NONE && metrics.processed_ns >= metrics.total_ns;
  return snapshot;
}

hm::TerminalProgressGraphSnapshot PipelineApplication::build_progress_graph_snapshot(
    const std::vector<std::shared_ptr<HmApp>>& app_contexts) const {
  hm::TerminalProgressGraphSnapshot graph;
  std::set<std::string> node_names;
  auto add_node = [&graph, &node_names](std::string name, int degree, bool active = true) -> std::string {
    if (node_names.emplace(name).second) {
      graph.nodes.push_back({name, degree, active, std::nullopt});
      graph.order.push_back(name);
      graph.max_degree = std::max(graph.max_degree, degree);
    }
    return name;
  };
  auto add_edge = [&graph](const std::string& from, const std::string& to) {
    if (!from.empty() && !to.empty()) {
      graph.edges.push_back({from, to});
    }
  };

  const bool multi_app = app_contexts.size() > 1;
  for (const auto& app_ctx : app_contexts) {
    if (!app_ctx) {
      continue;
    }
    const std::string prefix = multi_app ? ("app" + std::to_string(app_ctx->index) + "/") : "";
    std::vector<std::string> source_nodes;
    for (guint i = 0; i < app_ctx->config.num_source_sub_bins; ++i) {
      const NvDsSourceConfig& source_config = app_ctx->config.multi_source_config[i];
      if (!source_config.enable) {
        continue;
      }
      source_nodes.push_back(add_node(prefix + "source" + std::to_string(i), 0));
    }

    std::string last_video = add_node(prefix + "streammux", 1);
    for (const std::string& source_node : source_nodes) {
      add_edge(source_node, last_video);
    }

    int degree = 2;
    auto add_video_stage = [&](bool enabled, const std::string& name) {
      if (!enabled) {
        return;
      }
      const std::string node = add_node(prefix + name, degree++);
      add_edge(last_video, node);
      last_video = node;
    };
    add_video_stage(app_ctx->config.hmplaycropper_config.enable, "hmplaycropper");
    add_video_stage(app_ctx->config.dsfieldmask_config.enable, "ds-fieldmask");
    add_video_stage(app_ctx->config.primary_gie_config.enable, "primary-gie");
    add_video_stage(app_ctx->config.tracker_config.enable, "tracker");
    add_video_stage(app_ctx->config.dsplaytracker_config.enable, "ds-playtracker");
    add_video_stage(app_ctx->config.hmsticher_config.enable, "hmstitcher");

    std::vector<std::pair<gint, std::string>> sink_nodes;
    const int sink_degree = degree;
    for (guint i = 0; i < app_ctx->config.num_sink_sub_bins; ++i) {
      const NvDsSinkSubBinConfig& sink_config = app_ctx->config.sink_bin_sub_bin_config[i];
      if (!sink_config.enable) {
        continue;
      }
      const std::string sink_name = add_node(
          prefix + "sink" + std::to_string(sink_config.sink_id) + ":" + hm::to_string(sink_config.type), sink_degree);
      sink_nodes.push_back({sink_config.sink_id, sink_name});
      add_edge(last_video, sink_name);
    }

    for (guint i = 0; i < MAX_SOURCE_BINS; ++i) {
      const NvDsHmAudioConfig& audio_config = app_ctx->config.hmaudio_config[i];
      if (!audio_config.enable) {
        continue;
      }
      const std::string audio_node = add_node(prefix + "hmaudio" + std::to_string(i), std::max(0, degree - 1));

      auto add_audio_sink_edge = [&](gint sink_id) {
        for (const auto& sink_item : sink_nodes) {
          if (sink_item.first == sink_id) {
            add_edge(audio_node, sink_item.second);
            return;
          }
        }
      };
      if (audio_config.dest == DEST_SINK) {
        add_audio_sink_edge(audio_config.sink_id);
      } else if (audio_config.dest == DEST_MULTI_SINK) {
        for (guint sink_index = 0; sink_index < MAX_SINK_BINS; ++sink_index) {
          if (audio_config.multi_sink_ids[sink_index] >= 0) {
            add_audio_sink_edge(audio_config.multi_sink_ids[sink_index]);
          }
        }
      } else {
        for (const auto& sink_item : sink_nodes) {
          add_edge(audio_node, sink_item.second);
        }
      }
    }
  }

  if (graph.nodes.empty()) {
    add_node("pipeline", 0, false);
  }
  graph.concurrency_current =
      static_cast<int>(std::count_if(graph.nodes.begin(), graph.nodes.end(), [](const auto& n) { return n.active; }));
  graph.concurrency_max = static_cast<int>(graph.nodes.size());
  graph.threaded = true;
  return graph;
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

  hm::PlaybackProgressMetrics progress_metrics = collect_progress_metrics(app_ctx);
  if (numf > 0) {
    double current_fps_sum = 0.0;
    double average_fps_sum = 0.0;
    guint valid_fps_samples = 0;
    for (guint fps_index = 0; fps_index < numf; ++fps_index) {
      if (!std::isfinite(fps_[fps_index]) || !std::isfinite(fps_avg_[fps_index])) {
        continue;
      }
      current_fps_sum += std::max(0.0, fps_[fps_index]);
      average_fps_sum += std::max(0.0, fps_avg_[fps_index]);
      ++valid_fps_samples;
    }
    if (valid_fps_samples > 0) {
      progress_metrics.output_fps = current_fps_sum / valid_fps_samples;
      progress_metrics.output_fps_average = average_fps_sum / valid_fps_samples;
    }
  }
  hm::PlaybackProgressMetrics aggregate_progress;
  const auto active_stage = stage_app_contexts_.find(current_stage_);
  const size_t active_instances = active_stage == stage_app_contexts_.end() ? 0 : active_stage->second.size();
  auto& stage_progress = ui_progress_by_stage_[current_stage_];
  stage_progress[app_ctx->index] = progress_metrics;
  std::vector<hm::PlaybackProgressMetrics> instance_progress;
  instance_progress.reserve(active_instances);
  if (active_stage != stage_app_contexts_.end()) {
    for (const auto& active_context : active_stage->second) {
      const auto found = stage_progress.find(active_context->index);
      if (found != stage_progress.end()) {
        instance_progress.push_back(found->second);
      }
    }
  }
  const bool have_aggregate = instance_progress.size() == active_instances &&
      hm::aggregate_playback_progress(instance_progress, &aggregate_progress);
  if (std::getenv("HSTREAM_UI_PARENT_PID") && have_aggregate) {
    auto append_time = [](std::ostringstream& output, uint64_t value) {
      if (value == GST_CLOCK_TIME_NONE) {
        output << "unknown";
      } else {
        output << value;
      }
    };
    std::ostringstream ui_progress;
    ui_progress << "HSTREAM_PROGRESS processed_ns=";
    append_time(ui_progress, aggregate_progress.processed_ns);
    ui_progress << " total_ns=";
    append_time(ui_progress, aggregate_progress.total_ns);
    ui_progress << " remaining_ns=";
    append_time(ui_progress, aggregate_progress.remaining_ns);
    ui_progress << " eta_ns=";
    append_time(ui_progress, aggregate_progress.eta_ns);
    ui_progress << " speed_x=" << std::fixed << std::setprecision(6) << aggregate_progress.speed_x
                << " fps=" << aggregate_progress.output_fps << " fps_avg=" << aggregate_progress.output_fps_average
                << " fraction=" << aggregate_progress.fraction << " stage=" << current_stage_
                << " instance=aggregate instances=" << active_instances
                << " generation=" << playback_progress_generation_;
    g_print("%s\n", ui_progress.str().c_str());
  }
  if (progress_ui_ && progress_ui_->started()) {
    progress_ui_->update(make_terminal_progress_snapshot(app_ctx, str, progress_metrics));
    if (progress_metrics.valid) {
      g_mutex_unlock(&fps_lock_);
      return;
    }
  }

  if (header_print_cnt % 20 == 0) {
    g_print("\n**PERF:  ");
    for (i = 0; i < numf; i++) {
      if (str->aggregate_output_fps && numf == 1) {
        g_print("Output FPS (Avg)\t");
      } else {
        g_print("FPS %d (Avg)\t", i);
      }
    }
    g_print("Progress\tRemaining\tETA\tSpeed\t");
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
  const std::string progress_status = format_progress_status(progress_metrics);
  if (!progress_status.empty()) {
    g_print("%s", progress_status.c_str());
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
  std::chrono::steady_clock::time_point last_progress_wall;
  {
    std::lock_guard<std::mutex> lock(playback_timing_mu_);
    last_progress_wall = timed_run_last_progress_wall_;
  }
  if (time_limit_seconds_ > 0 &&
      hm::pipeline_internal::stitch_frame_should_account_playback(
          stitch_frame_calibration_active_.load(std::memory_order_acquire)) &&
      last_progress_wall != std::chrono::steady_clock::time_point{}) {
    constexpr int kTimedRunNoProgressTimeoutSeconds = 60;
    const auto stalled_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - last_progress_wall).count();
    if (stalled_seconds >= kTimedRunNoProgressTimeoutSeconds) {
      g_printerr(
          "Timed run saw no video-time progress for %d wall-clock seconds; stopping\n",
          kTimedRunNoProgressTimeoutSeconds);
      quit_ = TRUE;
      if (main_loop_) {
        g_main_loop_quit(main_loop_);
      }
      return FALSE;
    }
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
      "\tr: Resume\n"
      "\t@set-preview-active <program|stitched|sourceN|none> <generation>: Activate one GPU-native UI preview\n"
      "\t@set-render-window <xid>: Move the embedded program render sink to another X11 window\n"
      "\t@capture-preview-frame <program|main|stitched|sourceN> <image-path>: Save one diagnostic UI preview "
      "frame\n"
      "\t@set-render-audio-muted <0|1>: Mute or restore only the local render-audio branch\n"
      "\t@reset-progress-rate <generation>: Reset playback speed and ETA sampling after a process pause\n"
      "\t@set-property <element> <property=value>: Set an allowlisted runtime GStreamer property\n"
      "\t@set-properties <element property=value;...>: Atomically set allowlisted runtime properties\n\n");
  if (!stage_app_contexts_.empty() && !stage_app_contexts_.at(current_stage_).empty() &&
      stage_app_contexts_.at(current_stage_)[0] &&
      stage_app_contexts_.at(current_stage_)[0]->config.tiled_display_config.enable) {
    g_print(
        "NOTE: To expand a source in the 2D tiled display and view object details,\n"
        "      left-click on the source.\n"
        "      To go back to the tiled display, right-click anywhere on the window.\n\n");
  }
}

bool PipelineApplication::read_stdin_char(char* out) const {
  if (!out || !kbhit()) {
    return false;
  }
  char c = 0;
  const ssize_t bytes = ::read(STDIN_FILENO, &c, 1);
  if (bytes != 1) {
    return false;
  }
  *out = c;
  return true;
}

bool PipelineApplication::read_runtime_command_line(std::string* line) {
  char c = 0;
  while (read_stdin_char(&c)) {
    if (c == '\n' || c == '\r') {
      if (line) {
        *line = runtime_command_buffer_;
      }
      runtime_command_buffer_.clear();
      runtime_command_active_ = false;
      return true;
    }
    if (runtime_command_buffer_.size() >= 4096) {
      g_printerr("runtime command failed: command too long\n");
      runtime_command_buffer_.clear();
      runtime_command_active_ = false;
      return false;
    }
    runtime_command_buffer_.push_back(c);
  }
  return false;
}

namespace {
bool is_allowlisted_runtime_property(const std::string& element_name, const std::string& property_name) {
  return (element_name == "hmstitcher0" && property_name == "post-stitch-rotate-degrees") ||
      (element_name == "dsplaytracker0" &&
       (property_name == "runtime-tuning-config-file" || property_name == "fixed-edge-rotation-angle" ||
        property_name == "fixed-edge-rotation-angle-left" || property_name == "fixed-edge-rotation-angle-right" ||
        property_name == "dynamic-acceleration-scaling")) ||
      ((element_name == "playcropper0" || element_name == "playcropper") &&
       (property_name == "fixed-edge-rotation-angle" || property_name == "fixed-edge-rotation-angle-left" ||
        property_name == "fixed-edge-rotation-angle-right"));
}

std::string trim_ascii(std::string value) {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string::npos) {
    return "";
  }
  const auto last = value.find_last_not_of(" \t");
  return value.substr(first, last - first + 1);
}

bool parse_runtime_set_property(
    const std::string& line,
    std::string* element_name,
    std::string* property_name,
    std::string* value) {
  constexpr absl::string_view kCommand = "set-property";
  const std::string trimmed = trim_ascii(line);
  if (trimmed.rfind(std::string(kCommand), 0) != 0 ||
      (trimmed.size() > kCommand.size() && !std::isspace(static_cast<unsigned char>(trimmed[kCommand.size()])))) {
    return false;
  }
  std::string rest = trim_ascii(trimmed.substr(kCommand.size()));
  const size_t element_end = rest.find_first_of(" \t");
  if (element_end == std::string::npos) {
    return false;
  }
  *element_name = rest.substr(0, element_end);
  rest = trim_ascii(rest.substr(element_end + 1));
  const size_t equals = rest.find('=');
  if (equals == std::string::npos || equals == 0) {
    return false;
  }
  *property_name = trim_ascii(rest.substr(0, equals));
  *value = trim_ascii(rest.substr(equals + 1));
  return !element_name->empty() && !property_name->empty();
}

bool parse_runtime_set_properties(
    const std::string& line,
    std::vector<std::tuple<std::string, std::string, std::string>>* assignments) {
  constexpr absl::string_view kCommand = "set-properties";
  const std::string trimmed = trim_ascii(line);
  if (!assignments || trimmed.rfind(std::string(kCommand), 0) != 0 ||
      (trimmed.size() > kCommand.size() && !std::isspace(static_cast<unsigned char>(trimmed[kCommand.size()])))) {
    return false;
  }
  assignments->clear();
  for (absl::string_view item_view : absl::StrSplit(trim_ascii(trimmed.substr(kCommand.size())), ';')) {
    const std::string item(item_view);
    std::string element_name;
    std::string property_name;
    std::string value;
    if (!parse_runtime_set_property("set-property " + item, &element_name, &property_name, &value)) {
      assignments->clear();
      return false;
    }
    assignments->emplace_back(std::move(element_name), std::move(property_name), std::move(value));
  }
  return !assignments->empty();
}

bool parse_double_strict(const std::string& value, gdouble* out) {
  if (!out || value.empty()) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const gdouble parsed = g_ascii_strtod(value.c_str(), &end);
  if (value.c_str() == end || errno == ERANGE || !std::isfinite(parsed)) {
    return false;
  }
  while (end && *end && std::isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  if (!end || *end != '\0') {
    return false;
  }
  *out = parsed;
  return true;
}

bool parse_int64_strict(const std::string& value, gint64* out) {
  if (!out || value.empty()) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const gint64 parsed = g_ascii_strtoll(value.c_str(), &end, 10);
  if (value.c_str() == end || errno == ERANGE) {
    return false;
  }
  while (end && *end && std::isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  if (!end || *end != '\0') {
    return false;
  }
  *out = parsed;
  return true;
}

bool parse_uint64_strict(const std::string& value, guint64* out) {
  if (!out || value.empty() || value[0] == '-') {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const guint64 parsed = g_ascii_strtoull(value.c_str(), &end, 10);
  if (value.c_str() == end || errno == ERANGE) {
    return false;
  }
  while (end && *end && std::isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  if (!end || *end != '\0') {
    return false;
  }
  *out = parsed;
  return true;
}

bool parse_runtime_set_render_window(const std::string& line, guint64* window_id) {
  constexpr absl::string_view kCommand = "set-render-window";
  const std::string trimmed = trim_ascii(line);
  if (trimmed.rfind(std::string(kCommand), 0) != 0 ||
      (trimmed.size() > kCommand.size() && !std::isspace(static_cast<unsigned char>(trimmed[kCommand.size()])))) {
    return false;
  }
  return parse_uint64_strict(trim_ascii(trimmed.substr(kCommand.size())), window_id) && *window_id > 0;
}

bool parse_runtime_set_preview_active(const std::string& line, std::string* channel, guint64* generation) {
  constexpr absl::string_view kCommand = "set-preview-active";
  if (!channel || !generation)
    return false;
  const std::string trimmed = trim_ascii(line);
  if (trimmed.rfind(std::string(kCommand), 0) != 0 || trimmed.size() <= kCommand.size() ||
      !std::isspace(static_cast<unsigned char>(trimmed[kCommand.size()]))) {
    return false;
  }
  const std::string arguments = trim_ascii(trimmed.substr(kCommand.size()));
  const size_t separator = arguments.find_first_of(" \t");
  if (separator == std::string::npos)
    return false;
  *channel = arguments.substr(0, separator);
  const std::string generation_text = trim_ascii(arguments.substr(separator));
  return !channel->empty() && parse_uint64_strict(generation_text, generation) && *generation > 0;
}

bool parse_runtime_capture_preview_frame(const std::string& line, std::string* channel, std::string* path) {
  constexpr absl::string_view kCommand = "capture-preview-frame";
  if (!channel || !path) {
    return false;
  }
  const std::string trimmed = trim_ascii(line);
  if (trimmed.rfind(std::string(kCommand), 0) != 0 || trimmed.size() <= kCommand.size() ||
      !std::isspace(static_cast<unsigned char>(trimmed[kCommand.size()]))) {
    return false;
  }
  const std::string arguments = trim_ascii(trimmed.substr(kCommand.size()));
  const size_t separator = arguments.find_first_of(" \t");
  if (separator == std::string::npos) {
    return false;
  }
  *channel = arguments.substr(0, separator);
  *path = trim_ascii(arguments.substr(separator));
  return !channel->empty() && !path->empty();
}

enum class PreviewFrameSaveStatus {
  kSaved,
  kUnavailable,
  kFailed,
};

struct PreviewFrameSaveResult {
  PreviewFrameSaveStatus status{PreviewFrameSaveStatus::kFailed};
  int width{0};
  int height{0};
  std::string message;
};

PreviewFrameSaveResult save_preview_frame(GstElement* sink, const fs::path& output_path) {
  // Explicit one-shot diagnostic command only. This intentionally maps one
  // retained sample to host memory and is never used by hstream-ui or any
  // steady-state preview path.
  if (!sink || !g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "last-sample")) {
    return {PreviewFrameSaveStatus::kUnavailable};
  }
  GstSample* sample = nullptr;
  g_object_get(G_OBJECT(sink), "last-sample", &sample, nullptr);
  if (!sample) {
    return {PreviewFrameSaveStatus::kUnavailable};
  }
  auto release_sample = absl::MakeCleanup([sample] { gst_sample_unref(sample); });
  GstCaps* caps = gst_sample_get_caps(sample);
  GstBuffer* buffer = gst_sample_get_buffer(sample);
  GstVideoInfo info;
  if (!caps || !buffer || !gst_video_info_from_caps(&info, caps)) {
    return {PreviewFrameSaveStatus::kFailed, 0, 0, "invalid sample"};
  }
  GstVideoFrame frame;
  if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
    return {PreviewFrameSaveStatus::kFailed, 0, 0, "sample map failed"};
  }
  auto unmap_frame = absl::MakeCleanup([&frame] { gst_video_frame_unmap(&frame); });
  const int width = GST_VIDEO_INFO_WIDTH(&info);
  const int height = GST_VIDEO_INFO_HEIGHT(&info);
  const int row_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
  guint8* pixels = static_cast<guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
  if (!pixels || width <= 0 || height <= 0 || row_stride <= 0) {
    return {PreviewFrameSaveStatus::kFailed, 0, 0, "invalid frame layout"};
  }

  cv::Mat bgr;
  switch (GST_VIDEO_INFO_FORMAT(&info)) {
    case GST_VIDEO_FORMAT_BGRx:
    case GST_VIDEO_FORMAT_BGRA: {
      const cv::Mat source(height, width, CV_8UC4, pixels, static_cast<size_t>(row_stride));
      cv::cvtColor(source, bgr, cv::COLOR_BGRA2BGR);
      break;
    }
    case GST_VIDEO_FORMAT_RGBx:
    case GST_VIDEO_FORMAT_RGBA: {
      const cv::Mat source(height, width, CV_8UC4, pixels, static_cast<size_t>(row_stride));
      cv::cvtColor(source, bgr, cv::COLOR_RGBA2BGR);
      break;
    }
    case GST_VIDEO_FORMAT_BGR: {
      const cv::Mat source(height, width, CV_8UC3, pixels, static_cast<size_t>(row_stride));
      bgr = source.clone();
      break;
    }
    case GST_VIDEO_FORMAT_RGB: {
      const cv::Mat source(height, width, CV_8UC3, pixels, static_cast<size_t>(row_stride));
      cv::cvtColor(source, bgr, cv::COLOR_RGB2BGR);
      break;
    }
    default:
      return {PreviewFrameSaveStatus::kFailed, 0, 0, "unsupported frame format"};
  }

  constexpr int kMaximumPreviewWidth = 1600;
  constexpr int kMaximumPreviewHeight = 900;
  const double scale = std::min(
      1.0,
      std::min(static_cast<double>(kMaximumPreviewWidth) / width, static_cast<double>(kMaximumPreviewHeight) / height));
  cv::Mat preview = bgr;
  if (scale < 1.0) {
    cv::resize(
        bgr,
        preview,
        cv::Size(
            std::max(1, static_cast<int>(std::round(width * scale))),
            std::max(1, static_cast<int>(std::round(height * scale)))),
        0.0,
        0.0,
        cv::INTER_AREA);
  }
  try {
    if (!output_path.is_absolute() || !output_path.has_parent_path() || !fs::is_directory(output_path.parent_path())) {
      return {PreviewFrameSaveStatus::kFailed, 0, 0, "output directory is unavailable"};
    }
    const std::vector<int> jpeg_options = {cv::IMWRITE_JPEG_QUALITY, 82};
    if (!cv::imwrite(output_path.string(), preview, jpeg_options)) {
      return {PreviewFrameSaveStatus::kFailed, 0, 0, "JPEG write failed"};
    }
  } catch (const std::exception& exception) {
    return {PreviewFrameSaveStatus::kFailed, 0, 0, exception.what()};
  }
  return {PreviewFrameSaveStatus::kSaved, preview.cols, preview.rows};
}

PreviewFrameSaveResult save_gpu_preview_frame(GstElement* sink, const fs::path& output_path) {
  // Explicit one-shot E2E/diagnostic readback only. The steady-state renderer
  // continues to use CUDA/OpenGL interop without mapping pixel planes to CPU.
  std::vector<std::uint8_t> rgba;
  unsigned width = 0;
  unsigned height = 0;
  std::string capture_error;
  if (!hm::gpu_preview::capture_presented_frame(sink, &rgba, &width, &height, &capture_error)) {
    return {PreviewFrameSaveStatus::kUnavailable, 0, 0, capture_error};
  }
  if (width == 0 || height == 0 || rgba.size() != static_cast<size_t>(width) * height * 4U) {
    return {PreviewFrameSaveStatus::kFailed, 0, 0, "invalid GPU diagnostic frame"};
  }
  try {
    if (!output_path.is_absolute() || !output_path.has_parent_path() || !fs::is_directory(output_path.parent_path())) {
      return {PreviewFrameSaveStatus::kFailed, 0, 0, "output directory is unavailable"};
    }
    const cv::Mat source(static_cast<int>(height), static_cast<int>(width), CV_8UC4, rgba.data());
    cv::Mat bgr;
    cv::cvtColor(source, bgr, cv::COLOR_RGBA2BGR);
    if (!cv::imwrite(output_path.string(), bgr)) {
      return {PreviewFrameSaveStatus::kFailed, 0, 0, "diagnostic image write failed"};
    }
  } catch (const std::exception& exception) {
    return {PreviewFrameSaveStatus::kFailed, 0, 0, exception.what()};
  }
  return {PreviewFrameSaveStatus::kSaved, static_cast<int>(width), static_cast<int>(height)};
}

void report_last_render_sample(GstElement* sink) {
  GstSample* sample = nullptr;
  g_object_get(G_OBJECT(sink), "last-sample", &sample, nullptr);
  if (!sample) {
    g_print("runtime render sample unavailable\n");
    return;
  }
  auto release_sample = absl::MakeCleanup([sample] { gst_sample_unref(sample); });
  GstCaps* caps = gst_sample_get_caps(sample);
  GstBuffer* buffer = gst_sample_get_buffer(sample);
  GstVideoInfo info;
  if (!caps || !buffer || !gst_video_info_from_caps(&info, caps)) {
    g_print("runtime render sample invalid\n");
    return;
  }
  GstVideoFrame frame;
  if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
    g_print("runtime render sample map failed\n");
    return;
  }
  auto unmap_frame = absl::MakeCleanup([&frame] { gst_video_frame_unmap(&frame); });
  const guint pixel_stride = GST_VIDEO_INFO_COMP_PSTRIDE(&info, 0);
  const guint8* pixels = static_cast<const guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
  const gint row_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
  double luminance_sum = 0.0;
  size_t sample_count = 0;
  guint8 minimum_luminance = 255;
  guint8 maximum_luminance = 0;
  if (pixels && pixel_stride >= 3) {
    for (gint y = 0; y < GST_VIDEO_INFO_HEIGHT(&info); y += 16) {
      const guint8* row = pixels + static_cast<ptrdiff_t>(y) * row_stride;
      for (gint x = 0; x < GST_VIDEO_INFO_WIDTH(&info); x += 16) {
        const guint8* pixel = row + static_cast<size_t>(x) * pixel_stride;
        const guint8 luminance = static_cast<guint8>((pixel[0] + pixel[1] + pixel[2]) / 3);
        luminance_sum += luminance;
        minimum_luminance = std::min(minimum_luminance, luminance);
        maximum_luminance = std::max(maximum_luminance, luminance);
        ++sample_count;
      }
    }
  }
  g_print(
      "runtime render sample format=%s width=%u height=%u mean=%.2f range=%u\n",
      gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(&info)),
      GST_VIDEO_INFO_WIDTH(&info),
      GST_VIDEO_INFO_HEIGHT(&info),
      sample_count ? luminance_sum / sample_count : 0.0,
      sample_count ? static_cast<guint>(maximum_luminance - minimum_luminance) : 0);
}

bool set_gvalue_from_string(GValue* gvalue, GParamSpec* pspec, const std::string& value) {
  if (!gvalue || !pspec) {
    return false;
  }
  const GType value_type = G_PARAM_SPEC_VALUE_TYPE(pspec);
  g_value_init(gvalue, value_type);
  if (value_type == G_TYPE_STRING) {
    g_value_set_string(gvalue, value.c_str());
    return true;
  }
  if (value_type == G_TYPE_BOOLEAN) {
    if (value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "on") {
      g_value_set_boolean(gvalue, true);
      return true;
    }
    if (value == "0" || value == "false" || value == "FALSE" || value == "no" || value == "off") {
      g_value_set_boolean(gvalue, false);
      return true;
    }
    return false;
  }
  if (value_type == G_TYPE_DOUBLE) {
    gdouble parsed = 0.0;
    if (!parse_double_strict(value, &parsed)) {
      return false;
    }
    g_value_set_double(gvalue, parsed);
    return true;
  }
  if (value_type == G_TYPE_FLOAT) {
    gdouble parsed = 0.0;
    if (!parse_double_strict(value, &parsed)) {
      return false;
    }
    g_value_set_float(gvalue, static_cast<gfloat>(parsed));
    return true;
  }
  if (value_type == G_TYPE_INT) {
    gint64 parsed = 0;
    if (!parse_int64_strict(value, &parsed) || parsed < G_MININT || parsed > G_MAXINT) {
      return false;
    }
    g_value_set_int(gvalue, static_cast<gint>(parsed));
    return true;
  }
  if (value_type == G_TYPE_UINT) {
    guint64 parsed = 0;
    if (!parse_uint64_strict(value, &parsed) || parsed > G_MAXUINT) {
      return false;
    }
    g_value_set_uint(gvalue, static_cast<guint>(parsed));
    return true;
  }
  if (value_type == G_TYPE_INT64) {
    gint64 parsed = 0;
    if (!parse_int64_strict(value, &parsed)) {
      return false;
    }
    g_value_set_int64(gvalue, parsed);
    return true;
  }
  if (value_type == G_TYPE_UINT64) {
    guint64 parsed = 0;
    if (!parse_uint64_strict(value, &parsed)) {
      return false;
    }
    g_value_set_uint64(gvalue, parsed);
    return true;
  }
  return false;
}
} // namespace

bool PipelineApplication::set_render_audio_muted_runtime(bool muted) {
  size_t updated = 0;
  auto& app_contexts = stage_app_contexts_.at(current_stage_);
  for (const auto& app : app_contexts) {
    if (!app || !app->pipeline.pipeline) {
      continue;
    }
    for (size_t instance_id = 0; instance_id < MAX_SOURCE_BINS; ++instance_id) {
      GstElement* hmaudio_bin = app->pipeline.instance_bins[instance_id].hmaudio_bin.bin;
      if (!hmaudio_bin) {
        continue;
      }
      for (size_t sink_id = 0; sink_id < MAX_SINK_BINS; ++sink_id) {
        const std::string element_name = "hmaudio_render_sink" + std::to_string(sink_id) + "_volume";
        GstElement* volume = gst_bin_get_by_name(GST_BIN(hmaudio_bin), element_name.c_str());
        if (!volume) {
          continue;
        }
        g_object_set(G_OBJECT(volume), "mute", muted ? TRUE : FALSE, nullptr);
        gst_object_unref(volume);
        ++updated;
      }
    }
  }
  if (updated == 0) {
    g_printerr("runtime command failed: no local render-audio branch is active\n");
    return false;
  }
  g_print("HSTREAM_RENDER_AUDIO muted=%d branches=%zu\n", muted ? 1 : 0, updated);
  return true;
}

bool PipelineApplication::set_element_property_runtime(
    const std::string& element_name,
    const std::string& property_name,
    const std::string& value) {
  if (element_name.empty() || property_name.empty()) {
    g_printerr("runtime command failed: missing element or property\n");
    return false;
  }
  if (!is_allowlisted_runtime_property(element_name, property_name)) {
    g_printerr(
        "runtime command failed: property is not live-mutable here: %s.%s\n",
        element_name.c_str(),
        property_name.c_str());
    return false;
  }
  auto& app_ctx = stage_app_contexts_.at(current_stage_);
  for (const auto& app : app_ctx) {
    if (!app || !app->pipeline.pipeline) {
      continue;
    }
    GstElement* element = gst_bin_get_by_name(GST_BIN(app->pipeline.pipeline), element_name.c_str());
    if (!element) {
      continue;
    }
    GParamSpec* pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(element), property_name.c_str());
    if (!pspec) {
      gst_object_unref(element);
      g_printerr(
          "runtime command failed: element %s has no property %s\n", element_name.c_str(), property_name.c_str());
      return false;
    }
    GValue gvalue = G_VALUE_INIT;
    if (!set_gvalue_from_string(&gvalue, pspec, value)) {
      if (G_IS_VALUE(&gvalue)) {
        g_value_unset(&gvalue);
      }
      gst_object_unref(element);
      g_printerr(
          "runtime command failed: unsupported property type for %s.%s\n", element_name.c_str(), property_name.c_str());
      return false;
    }
    if (g_param_value_validate(pspec, &gvalue)) {
      g_value_unset(&gvalue);
      gst_object_unref(element);
      g_printerr(
          "runtime command failed: value out of range for %s.%s=%s\n",
          element_name.c_str(),
          property_name.c_str(),
          value.c_str());
      return false;
    }
    g_object_set_property(G_OBJECT(element), property_name.c_str(), &gvalue);
    g_value_unset(&gvalue);
    GParamSpec* status_pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(element), "last-property-set-ok");
    if (status_pspec && G_PARAM_SPEC_VALUE_TYPE(status_pspec) == G_TYPE_BOOLEAN) {
      gboolean accepted = TRUE;
      g_object_get(G_OBJECT(element), "last-property-set-ok", &accepted, nullptr);
      if (!accepted) {
        gst_object_unref(element);
        g_printerr(
            "runtime command failed: plugin rejected %s.%s=%s\n",
            element_name.c_str(),
            property_name.c_str(),
            value.c_str());
        return false;
      }
    }
    gst_object_unref(element);
    g_print("runtime property %s %s=%s\n", element_name.c_str(), property_name.c_str(), value.c_str());
    return true;
  }
  g_printerr("runtime command failed: element not found: %s\n", element_name.c_str());
  return false;
}

bool PipelineApplication::set_element_properties_runtime(
    const std::vector<std::tuple<std::string, std::string, std::string>>& assignments) {
  struct PendingAssignment {
    GstElement* element{nullptr};
    std::string element_name;
    std::string property_name;
    std::string value;
    GParamSpec* pspec{nullptr};
    GValue requested = G_VALUE_INIT;
    GValue previous = G_VALUE_INIT;
  };
  std::vector<PendingAssignment> pending;
  auto cleanup = absl::MakeCleanup([&]() {
    for (PendingAssignment& assignment : pending) {
      if (G_IS_VALUE(&assignment.requested)) {
        g_value_unset(&assignment.requested);
      }
      if (G_IS_VALUE(&assignment.previous)) {
        g_value_unset(&assignment.previous);
      }
      if (assignment.element) {
        gst_object_unref(assignment.element);
      }
    }
  });
  auto& app_ctx = stage_app_contexts_.at(current_stage_);
  for (const auto& [element_name, property_name, value] : assignments) {
    if (!is_allowlisted_runtime_property(element_name, property_name)) {
      g_printerr(
          "runtime command failed: property is not live-mutable here: %s.%s\n",
          element_name.c_str(),
          property_name.c_str());
      return false;
    }
    GstElement* element = nullptr;
    for (const auto& app : app_ctx) {
      if (app && app->pipeline.pipeline &&
          (element = gst_bin_get_by_name(GST_BIN(app->pipeline.pipeline), element_name.c_str()))) {
        break;
      }
    }
    if (!element) {
      g_printerr("runtime command failed: element not found: %s\n", element_name.c_str());
      return false;
    }
    PendingAssignment assignment;
    assignment.element = element;
    assignment.element_name = element_name;
    assignment.property_name = property_name;
    assignment.value = value;
    assignment.pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(element), property_name.c_str());
    if (!assignment.pspec || !set_gvalue_from_string(&assignment.requested, assignment.pspec, value) ||
        g_param_value_validate(assignment.pspec, &assignment.requested)) {
      pending.push_back(std::move(assignment));
      g_printerr(
          "runtime command failed: invalid value for %s.%s=%s\n",
          element_name.c_str(),
          property_name.c_str(),
          value.c_str());
      return false;
    }
    if (assignments.size() > 1 && (assignment.pspec->flags & G_PARAM_READABLE) == 0) {
      pending.push_back(std::move(assignment));
      g_printerr(
          "runtime command failed: property has no rollback snapshot: %s.%s\n",
          element_name.c_str(),
          property_name.c_str());
      return false;
    }
    g_value_init(&assignment.previous, G_PARAM_SPEC_VALUE_TYPE(assignment.pspec));
    if ((assignment.pspec->flags & G_PARAM_READABLE) != 0) {
      g_object_get_property(G_OBJECT(element), property_name.c_str(), &assignment.previous);
    }
    pending.push_back(std::move(assignment));
  }
  size_t applied = 0;
  for (; applied < pending.size(); ++applied) {
    PendingAssignment& assignment = pending[applied];
    g_object_set_property(G_OBJECT(assignment.element), assignment.property_name.c_str(), &assignment.requested);
    GParamSpec* status = g_object_class_find_property(G_OBJECT_GET_CLASS(assignment.element), "last-property-set-ok");
    gboolean accepted = TRUE;
    if (status && G_PARAM_SPEC_VALUE_TYPE(status) == G_TYPE_BOOLEAN) {
      g_object_get(G_OBJECT(assignment.element), "last-property-set-ok", &accepted, nullptr);
    }
    if (!accepted) {
      break;
    }
  }
  if (applied != pending.size()) {
    const size_t failed_index = applied;
    while (applied > 0) {
      --applied;
      PendingAssignment& assignment = pending[applied];
      g_object_set_property(G_OBJECT(assignment.element), assignment.property_name.c_str(), &assignment.previous);
    }
    const PendingAssignment& failed = pending[failed_index];
    g_printerr(
        "runtime command failed: plugin rejected %s.%s=%s\n",
        failed.element_name.c_str(),
        failed.property_name.c_str(),
        failed.value.c_str());
    return false;
  }
  for (const PendingAssignment& assignment : pending) {
    g_print(
        "runtime property %s %s=%s\n",
        assignment.element_name.c_str(),
        assignment.property_name.c_str(),
        assignment.value.c_str());
  }
  return true;
}

bool PipelineApplication::handle_runtime_command_line(const std::string& line) {
  constexpr absl::string_view kResetProgressCommand = "reset-progress-rate";
  const std::string trimmed_line = trim_ascii(line);
  constexpr absl::string_view kRenderAudioMutedCommand = "set-render-audio-muted";
  if (trimmed_line.rfind(std::string(kRenderAudioMutedCommand), 0) == 0 &&
      trimmed_line.size() > kRenderAudioMutedCommand.size() &&
      std::isspace(static_cast<unsigned char>(trimmed_line[kRenderAudioMutedCommand.size()]))) {
    const std::string value = trim_ascii(trimmed_line.substr(kRenderAudioMutedCommand.size()));
    if (value != "0" && value != "1") {
      g_printerr("runtime command failed: set-render-audio-muted requires 0 or 1\n");
      return false;
    }
    return set_render_audio_muted_runtime(value == "1");
  }
  if (trimmed_line.rfind(std::string(kResetProgressCommand), 0) == 0 &&
      trimmed_line.size() > kResetProgressCommand.size() &&
      std::isspace(static_cast<unsigned char>(trimmed_line[kResetProgressCommand.size()]))) {
    guint64 generation = 0;
    if (!parse_uint64_strict(trim_ascii(trimmed_line.substr(kResetProgressCommand.size())), &generation) ||
        generation == 0) {
      g_printerr("runtime command failed: reset-progress-rate requires a positive generation\n");
      return false;
    }
    reset_playback_progress_rates(generation);
    return true;
  }
  std::string active_preview_channel;
  guint64 preview_generation = 0;
  if (parse_runtime_set_preview_active(line, &active_preview_channel, &preview_generation)) {
    return set_preview_active_runtime(active_preview_channel, preview_generation);
  }
  guint64 window_id = 0;
  if (parse_runtime_set_render_window(line, &window_id)) {
    return set_render_window_runtime(window_id);
  }
  std::string preview_channel;
  std::string preview_path;
  if (parse_runtime_capture_preview_frame(line, &preview_channel, &preview_path)) {
    return capture_preview_frame_runtime(preview_channel, preview_path);
  }
  std::vector<std::tuple<std::string, std::string, std::string>> assignments;
  if (parse_runtime_set_properties(line, &assignments)) {
    return set_element_properties_runtime(assignments);
  }
  std::string element_name;
  std::string property_name;
  std::string value;
  if (!parse_runtime_set_property(line, &element_name, &property_name, &value)) {
    g_printerr(
        "runtime command failed: expected: set-preview-active <program|stitched|sourceN|none> <generation>, "
        "set-render-window <xid>, set-render-audio-muted <0|1>, capture-preview-frame <main|stitched|sourceN> "
        "<jpg-path>, set-properties "
        "<element property=value;...>, reset-progress-rate <generation>, or set-property <element> <property=value>\n");
    return false;
  }
  return set_element_property_runtime(element_name, property_name, value);
}

void PipelineApplication::reset_playback_progress_rates(uint64_t generation) {
  g_mutex_lock(&fps_lock_);
  const bool stale = generation < playback_progress_generation_;
  if (generation > playback_progress_generation_) {
    playback_progress_generation_ = generation;
    for (auto& [index, state] : progress_states_) {
      (void)index;
      state.rate_estimator.reset();
    }
    ui_progress_by_stage_.erase(current_stage_);
  }
  const size_t active_instances = stage_app_contexts_.at(current_stage_).size();
  const uint64_t active_generation = playback_progress_generation_;
  g_mutex_unlock(&fps_lock_);
  if (stale) {
    g_print(
        "HSTREAM_PROGRESS status=stale-reset generation=%" G_GUINT64_FORMAT " active_generation=%" G_GUINT64_FORMAT
        " stage=%ld instance=aggregate instances=%zu\n",
        generation,
        active_generation,
        current_stage_,
        active_instances);
    return;
  }
  g_print(
      "HSTREAM_PROGRESS status=reset generation=%" G_GUINT64_FORMAT " stage=%ld instance=aggregate instances=%zu\n",
      generation,
      current_stage_,
      active_instances);
}

bool PipelineApplication::set_preview_active_runtime(const std::string& channel, guint64 generation) {
  if (ui_preview_channels_.empty()) {
    g_printerr("runtime preview activation failed: GPU-native UI previews are not configured\n");
    return false;
  }
  if (generation <= active_ui_preview_generation_) {
    g_print(
        "HSTREAM_PREVIEW channel=%s status=stale generation=%" G_GUINT64_FORMAT
        " message=activation generation is not newer than %" G_GUINT64_FORMAT "\n",
        channel.c_str(),
        generation,
        active_ui_preview_generation_);
    return true;
  }

  const auto previous = ui_preview_channels_.find(active_ui_preview_channel_);
  if (previous != ui_preview_channels_.end() && previous->first == channel) {
    // A first-frame recovery for the selected channel must not destroy its
    // renderer or wait behind a buffer that is still negotiating downstream.
    // Advancing the generation re-arms the ready acknowledgement on the next
    // presented frame while leaving the observational GPU branch flowing.
    hm::gpu_preview::set_renderer_generation(previous->second.sink, generation);
    hm::gpu_preview::set_isolation_generation(previous->second.ingress_isolation, generation);
    hm::gpu_preview::set_isolation_generation(previous->second.isolation, generation);
    active_ui_preview_generation_ = generation;
    if (!hm::gpu_preview::isolation_active(previous->second.ingress_isolation) ||
        !hm::gpu_preview::isolation_active(previous->second.isolation)) {
      active_ui_preview_channel_.clear();
      g_print(
          "HSTREAM_PREVIEW channel=%s status=failed generation=%" G_GUINT64_FORMAT
          " message=the selected GPU preview channel cannot be re-armed\n",
          channel.c_str(),
          generation);
      return true;
    }
    g_print(
        "HSTREAM_PREVIEW channel=%s status=activated generation=%" G_GUINT64_FORMAT
        " message=GPU preview branch re-armed\n",
        channel.c_str(),
        generation);
    return true;
  }
  if (previous != ui_preview_channels_.end()) {
    // Close the cheap ingress gate first so no new buffer enters the queue.
    // Closing the post-queue barrier then waits for any buffer already inside
    // the converter/sink path. Only then is renderer destruction safe.
    hm::gpu_preview::set_isolation_active(previous->second.ingress_isolation, false, generation);
    hm::gpu_preview::set_isolation_active(previous->second.isolation, false, generation);
    if (!hm::gpu_preview::quiesce(previous->second.sink, generation)) {
      active_ui_preview_channel_.clear();
      active_ui_preview_generation_ = generation;
      g_print(
          "HSTREAM_PREVIEW channel=%s status=failed generation=%" G_GUINT64_FORMAT
          " message=could not quiesce the previous preview channel\n",
          channel.c_str(),
          generation);
      return true;
    }
  }

  active_ui_preview_channel_.clear();
  active_ui_preview_generation_ = generation;
  if (channel == "none") {
    g_print(
        "HSTREAM_PREVIEW channel=none status=deactivated generation=%" G_GUINT64_FORMAT
        " message=all GPU preview branches are inactive\n",
        generation);
    return true;
  }

  const auto target = ui_preview_channels_.find(channel);
  if (target == ui_preview_channels_.end()) {
    g_print(
        "HSTREAM_PREVIEW channel=%s status=unavailable generation=%" G_GUINT64_FORMAT
        " message=the requested camera source is unavailable\n",
        channel.c_str(),
        generation);
    return true;
  }

  g_object_set(G_OBJECT(target->second.sink), "generation", generation, nullptr);
  // Arm the drain barrier before opening the ingress gate. A newly enqueued
  // buffer can therefore never be dropped between the two gates.
  hm::gpu_preview::set_isolation_active(target->second.isolation, true, generation);
  hm::gpu_preview::set_isolation_active(target->second.ingress_isolation, true, generation);
  if (!hm::gpu_preview::isolation_active(target->second.isolation) ||
      !hm::gpu_preview::isolation_active(target->second.ingress_isolation)) {
    hm::gpu_preview::set_isolation_active(target->second.ingress_isolation, false, generation);
    hm::gpu_preview::set_isolation_active(target->second.isolation, false, generation);
    hm::gpu_preview::quiesce(target->second.sink, generation);
    g_print(
        "HSTREAM_PREVIEW channel=%s status=failed generation=%" G_GUINT64_FORMAT
        " message=the requested GPU preview channel has failed locally\n",
        channel.c_str(),
        generation);
    return true;
  }
  active_ui_preview_channel_ = channel;
  g_print(
      "HSTREAM_PREVIEW channel=%s status=activated generation=%" G_GUINT64_FORMAT
      " message=GPU preview branch activated\n",
      channel.c_str(),
      generation);
  return true;
}

bool PipelineApplication::capture_preview_frame_runtime(const std::string& channel, const std::string& path) {
  GstElement* sink = nullptr;
  bool release_sink = false;
  auto stage = stage_app_contexts_.find(current_stage_);
  if (stage == stage_app_contexts_.end()) {
    g_print("runtime preview frame unavailable channel=%s path=%s\n", channel.c_str(), path.c_str());
    return true;
  }

  const std::string gpu_channel = channel == "main" ? "program" : channel;
  const auto gpu_preview = ui_preview_channels_.find(gpu_channel);
  const bool gpu_native_capture = gpu_preview != ui_preview_channels_.end();
  if (gpu_native_capture) {
    sink = gpu_preview->second.sink;
  } else if (channel == "main") {
    for (const auto& app_context : stage->second) {
      if (!app_context)
        continue;
      NvDsSinkBin& output = app_context->pipeline.instance_bins[0].sink_bin;
      if (output.bin) {
        sink = gst_bin_get_by_name(GST_BIN(output.bin), "program_preview_sink");
        release_sink = sink != nullptr;
      }
      if (sink)
        break;
      for (guint sink_index = 0; sink_index < app_context->config.num_sink_sub_bins; ++sink_index) {
        GstElement* candidate = app_context->pipeline.instance_bins[0].sink_bin.sub_bins[sink_index].sink;
        if (GST_IS_VIDEO_OVERLAY(candidate) && !manages_its_own_window(candidate)) {
          sink = candidate;
          break;
        }
      }
      if (sink)
        break;
    }
  } else if (channel == "stitched") {
    for (const auto& app_context : stage->second) {
      if (app_context && app_context->pipeline.hmstitcher_bin.preview_sink) {
        sink = app_context->pipeline.hmstitcher_bin.preview_sink;
        break;
      }
    }
  } else if (channel.rfind("source", 0) == 0) {
    guint64 source_index = 0;
    if (parse_uint64_strict(channel.substr(6), &source_index)) {
      for (const auto& app_context : stage->second) {
        if (!app_context || source_index >= app_context->pipeline.multi_src_bin.num_bins)
          continue;
        sink = app_context->pipeline.multi_src_bin.sub_bins[source_index].fakesink;
        if (sink)
          break;
      }
    }
  }

  if (!sink) {
    g_print("runtime preview frame unavailable channel=%s path=%s\n", channel.c_str(), path.c_str());
    return true;
  }
  auto release_retained_sink = absl::MakeCleanup([&]() {
    if (release_sink)
      gst_object_unref(sink);
  });
  const PreviewFrameSaveResult result =
      gpu_native_capture ? save_gpu_preview_frame(sink, fs::path(path)) : save_preview_frame(sink, fs::path(path));
  if (result.status == PreviewFrameSaveStatus::kUnavailable) {
    g_print(
        "runtime preview frame unavailable channel=%s path=%s message=%s\n",
        channel.c_str(),
        path.c_str(),
        result.message.empty() ? "no retained frame" : result.message.c_str());
    return true;
  }
  if (result.status == PreviewFrameSaveStatus::kFailed) {
    g_print(
        "runtime preview frame failed channel=%s path=%s message=%s\n",
        channel.c_str(),
        path.c_str(),
        result.message.c_str());
    return true;
  }
  g_print(
      "runtime preview frame channel=%s path=%s width=%d height=%d\n",
      channel.c_str(),
      path.c_str(),
      result.width,
      result.height);
  return true;
}

bool PipelineApplication::set_render_window_runtime(guint64 window_id) {
  if (window_id == 0) {
    g_printerr("runtime render window failed: XID must be positive\n");
    return false;
  }
  size_t updated_sinks = 0;
  auto& app_contexts = stage_app_contexts_.at(current_stage_);
  for (const auto& app_context : app_contexts) {
    if (!app_context)
      continue;
    for (guint sink_index = 0; sink_index < app_context->config.num_sink_sub_bins; ++sink_index) {
      GstElement* sink = app_context->pipeline.instance_bins[0].sink_bin.sub_bins[sink_index].sink;
      if (!GST_IS_VIDEO_OVERLAY(sink) || manages_its_own_window(sink))
        continue;
      gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), static_cast<guintptr>(window_id));
      gst_video_overlay_expose(GST_VIDEO_OVERLAY(sink));
      report_last_render_sample(sink);
      ++updated_sinks;
    }
  }
  if (updated_sinks == 0) {
    g_printerr("runtime render window failed: no embedded render sink is active\n");
    return false;
  }
  render_window_id_ = static_cast<gint64>(window_id);
  auto windows = stage_windows_.find(current_stage_);
  if (windows != stage_windows_.end()) {
    for (const auto& app_context : app_contexts) {
      if (app_context && windows->second.count(app_context->index)) {
        windows->second[app_context->index] = static_cast<Window>(window_id);
      }
    }
  }
  g_print("runtime render window id=%" G_GUINT64_FORMAT " sinks=%zu\n", window_id, updated_sinks);
  return true;
}

gboolean PipelineApplication::event_thread_func_static(gpointer arg) {
  if (instance_)
    return instance_->event_thread_func();
  return TRUE;
}

uint64_t PipelineApplication::initial_pipeline_position_ns(const HmApp* app_ctx) const {
  return hm::pipeline_internal::stitch_frame_initial_position(
      start_time_ns_,
      stitch_frame_time_ns_,
      app_ctx && app_ctx->configurator().stitching_calibration_required(),
      app_ctx && stitch_frame_rewound_contexts_.count(app_ctx) != 0);
}

gboolean PipelineApplication::handle_element_message_static(AppCtx* app_ctx, GstMessage* message) {
  return instance_ ? instance_->handle_element_message(app_ctx, message) : FALSE;
}

gboolean PipelineApplication::handle_element_message(AppCtx* app_ctx, GstMessage* message) {
  const GstStructure* structure = message ? gst_message_get_structure(message) : nullptr;
  if (!structure || !gst_structure_has_name(structure, "hstream-stitching-calibration-complete")) {
    return FALSE;
  }
  const auto active_stage = stage_app_contexts_.find(current_stage_);
  if (!app_ctx || active_stage == stage_app_contexts_.end()) {
    return TRUE;
  }
  const auto completion_context =
      std::find_if(active_stage->second.begin(), active_stage->second.end(), [app_ctx](const auto& context) {
        return context.get() == app_ctx;
      });
  if (completion_context == active_stage->second.end()) {
    return TRUE;
  }
  const char* output_generation = gst_structure_get_string(structure, "output-generation");
  std::string stitcher_config_path;
  try {
    stitcher_config_path = hm::get_node_value(
        (*completion_context)->configurator().config(), "pipeline.hmstitcher.config-file", std::string());
  } catch (const std::exception& error) {
    g_printerr("Ignoring malformed stitching completion message config: %s\n", error.what());
    return TRUE;
  }
  if (!output_generation || !*output_generation || stitcher_config_path.empty() ||
      !hm::stitching::is_field_mask_configured(stitcher_config_path, output_generation)) {
    g_printerr("Ignoring stale stitching completion message for a non-current output generation\n");
    return TRUE;
  }
  // The stitch artifact generation and completion latch are shared across all
  // contexts in this stage. Only one stitcher posts the completion message.
  // Keep every calibration context classified as such until its old worker is
  // cancelled and its replacement pipeline is running.
  std::vector<hm::pipeline_internal::StitchFrameRewindState> states;
  states.reserve(active_stage->second.size());
  for (const auto& context : active_stage->second) {
    states.push_back({
        context && context->configurator().stitching_calibration_required(),
        context && stitch_frame_rewound_contexts_.count(context.get()) != 0,
        context && stitch_frame_rewind_pending_contexts_.count(context.get()) != 0,
    });
  }
  for (const size_t index : hm::pipeline_internal::stitch_frame_rewind_candidates(stitch_frame_time_ns_, states)) {
    AppCtx* context = active_stage->second[index].get();
    stitch_frame_rewind_pending_contexts_.insert(context);
  }
  if (!stitch_frame_rewind_pending_contexts_.empty() && stitch_frame_rewind_source_id_ == 0) {
    auto* request = new StitchFrameRewindRequest{
        .stage = current_stage_,
        .main_loop_generation = main_loop_generation_,
    };
    stitch_frame_rewind_source_id_ = g_timeout_add_full(
        G_PRIORITY_DEFAULT, 10, rewind_after_stitching_calibration_static, request, [](gpointer data) {
          delete static_cast<StitchFrameRewindRequest*>(data);
        });
    if (stitch_frame_rewind_source_id_ == 0) {
      delete request;
      stitch_frame_rewind_pending_contexts_.clear();
      g_printerr("Failed to schedule playback restart after stitching calibration\n");
      return TRUE;
    }
  } else if (
      stitch_frame_rewind_pending_contexts_.empty() &&
      stitch_frame_calibration_active_.load(std::memory_order_acquire)) {
    // A zero stitch-frame time needs no pipeline recreation, but calibration
    // work still must not consume normal playback progress or time limits.
    // Publish a fresh baseline before enabling those callbacks.
    reset_playback_timing_state(current_stage_);
    {
      std::lock_guard<std::mutex> lock(playback_timing_mu_);
      timed_run_last_progress_wall_ =
          time_limit_seconds_ > 0 ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    }
    for (const auto& context : active_stage->second) {
      if (context) {
        one_pass_calibration_contexts_.erase(context.get());
      }
    }
    stitch_frame_calibration_active_.store(false, std::memory_order_release);
  }
  return TRUE;
}

gboolean PipelineApplication::rewind_after_stitching_calibration_static(gpointer arg) {
  const auto* request = static_cast<const StitchFrameRewindRequest*>(arg);
  if (instance_ && request) {
    return instance_->rewind_after_stitching_calibration(request->stage, request->main_loop_generation);
  }
  return G_SOURCE_REMOVE;
}

void PipelineApplication::cancel_stitch_frame_rewind(uint64_t main_loop_generation) {
  if (main_loop_generation != main_loop_generation_) {
    return;
  }
  if (stitch_frame_rewind_source_id_ != 0) {
    g_source_remove(stitch_frame_rewind_source_id_);
    stitch_frame_rewind_source_id_ = 0;
  }
  stitch_frame_rewind_pending_contexts_.clear();
  stitch_frame_rewind_cancellation_requested_ = false;
  stitch_frame_rewind_deadline_ = {};
  stitch_frame_calibration_active_.store(false, std::memory_order_release);
}

gboolean PipelineApplication::rewind_after_stitching_calibration(long stage, uint64_t main_loop_generation) {
  if (!hm::pipeline_internal::stitch_frame_rewind_request_is_current(
          stage, main_loop_generation, current_stage_, main_loop_generation_, main_loop_ != nullptr)) {
    stitch_frame_rewind_source_id_ = 0;
    stitch_frame_rewind_pending_contexts_.clear();
    stitch_frame_rewind_cancellation_requested_ = false;
    stitch_frame_rewind_deadline_ = {};
    return FALSE;
  }
  const auto active_stage = stage_app_contexts_.find(stage);
  if (active_stage == stage_app_contexts_.end()) {
    stitch_frame_rewind_source_id_ = 0;
    stitch_frame_rewind_pending_contexts_.clear();
    stitch_frame_rewind_cancellation_requested_ = false;
    stitch_frame_rewind_deadline_ = {};
    return FALSE;
  }
  std::vector<AppCtx*> rewind_contexts;
  for (const auto& context : active_stage->second) {
    if (context && stitch_frame_rewind_pending_contexts_.count(context.get()) != 0 &&
        stitch_frame_rewound_contexts_.count(context.get()) == 0) {
      rewind_contexts.push_back(context.get());
    }
  }
  if (rewind_contexts.empty() || quit_) {
    stitch_frame_rewind_source_id_ = 0;
    stitch_frame_rewind_pending_contexts_.clear();
    stitch_frame_rewind_cancellation_requested_ = false;
    stitch_frame_rewind_deadline_ = {};
    return FALSE;
  }

  // A shared completion can arrive while another same-stage stitcher is still
  // inside synchronous feature, Hugin, or rink-mask work. Detach the old bus
  // watches before cooperative cancellation so an expected AbortedError from
  // those workers cannot fail the replacement generation. Releasing every URI
  // barrier first also prevents teardown from waiting on an abandoned pair.
  // Request PAUSED asynchronously and poll from this timeout source: setting a
  // busy streaming pipeline directly to NULL can block the GLib main loop.
  if (!stitch_frame_rewind_cancellation_requested_) {
    g_print(
        "hmstitcher: calibration frame complete; restarting playback at %" GST_TIME_FORMAT "\n",
        GST_TIME_ARGS(start_time_ns_));
    for (AppCtx* context : rewind_contexts) {
      if (context->pipeline.pipeline) {
        GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(context->pipeline.pipeline));
        if (bus) {
          gst_bus_remove_watch(bus);
          gst_object_unref(bus);
        }
      }
      cancel_uri_playlist_frame_barrier(&context->pipeline.multi_src_bin);
      if (GstElement* stitcher = context->pipeline.hmstitcher_bin.elem_hmstitcher) {
        g_object_set(G_OBJECT(stitcher), "cancel-pending-work", TRUE, nullptr);
      }
      context->eos_received = TRUE;
      if (context->pipeline.pipeline &&
          gst_element_set_state(context->pipeline.pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Failed to quiesce calibration pipeline before stitch-frame restart\n");
        context->return_value = -1;
        quit_ = TRUE;
        stitch_frame_rewind_source_id_ = 0;
        if (main_loop_)
          g_main_loop_quit(main_loop_);
        return FALSE;
      }
    }
    stitch_frame_rewind_cancellation_requested_ = true;
    stitch_frame_rewind_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  }

  bool all_quiesced = true;
  for (AppCtx* context : rewind_contexts) {
    if (!context->pipeline.pipeline) {
      continue;
    }
    GstState current = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    const GstStateChangeReturn state =
        gst_element_get_state(context->pipeline.pipeline, &current, &pending, /*timeout=*/0);
    if (state == GST_STATE_CHANGE_FAILURE) {
      g_printerr("Calibration pipeline failed while quiescing for stitch-frame restart\n");
      context->return_value = -1;
      quit_ = TRUE;
      stitch_frame_rewind_source_id_ = 0;
      if (main_loop_)
        g_main_loop_quit(main_loop_);
      return FALSE;
    }
    if (state == GST_STATE_CHANGE_ASYNC || current > GST_STATE_PAUSED || pending > GST_STATE_PAUSED) {
      all_quiesced = false;
    }
  }
  if (!all_quiesced) {
    if (std::chrono::steady_clock::now() < stitch_frame_rewind_deadline_) {
      return G_SOURCE_CONTINUE;
    }
    g_printerr("Timed out quiescing calibration peers before stitch-frame restart\n");
    for (AppCtx* context : rewind_contexts) {
      context->return_value = -1;
    }
    quit_ = TRUE;
    stitch_frame_rewind_source_id_ = 0;
    if (main_loop_)
      g_main_loop_quit(main_loop_);
    return FALSE;
  }
  stitch_frame_rewind_source_id_ = 0;
  stitch_frame_rewind_cancellation_requested_ = false;
  stitch_frame_rewind_deadline_ = {};

  std::vector<AppCtx*> paused_contexts;
  paused_contexts.reserve(active_stage->second.size());
  for (const auto& context : active_stage->second) {
    if (!context || !context->pipeline.pipeline ||
        std::find(rewind_contexts.begin(), rewind_contexts.end(), context.get()) != rewind_contexts.end()) {
      continue;
    }
    if (!pause_pipeline(context.get())) {
      g_printerr("Failed to pause pipeline before stitch-frame restart\n");
      context->return_value = -1;
      quit_ = TRUE;
      if (main_loop_)
        g_main_loop_quit(main_loop_);
      return FALSE;
    }
    paused_contexts.push_back(context.get());
  }

  for (AppCtx* context : rewind_contexts) {
    stitch_frame_rewound_contexts_.insert(context);
  }
  reset_playback_timing_state(stage);

  bool recreation_ok = true;
  for (AppCtx* context : rewind_contexts) {
    stitch_frame_rewind_pending_contexts_.erase(context);
    if (!recreate_pipeline_thread_func(context)) {
      context->return_value = -1;
      context->quit = TRUE;
      recreation_ok = false;
      break;
    }
    one_pass_calibration_contexts_.erase(context);
  }
  for (AppCtx* context : paused_contexts) {
    if (std::find(rewind_contexts.begin(), rewind_contexts.end(), context) == rewind_contexts.end()) {
      recreation_ok = resume_pipeline(context) && recreation_ok;
    }
  }
  if (!recreation_ok) {
    quit_ = TRUE;
    if (main_loop_)
      g_main_loop_quit(main_loop_);
    return FALSE;
  }

  // The calibration pipelines were paused before the shared timing fields
  // were reset, and all callbacks continue to ignore those fields while this
  // flag is true. Publish the new baseline before allowing normal accounting.
  reset_playback_timing_state(stage);
  {
    std::lock_guard<std::mutex> lock(playback_timing_mu_);
    timed_run_last_progress_ns_ = GST_CLOCK_TIME_NONE;
    timed_run_last_progress_wall_ =
        time_limit_seconds_ > 0 ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  }
  stitch_frame_calibration_active_.store(false, std::memory_order_release);
  g_print("hmstitcher: playback restarted after stitch-frame calibration\n");
  return G_SOURCE_REMOVE;
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
  char c = 0;
  if (runtime_command_active_) {
    std::string line;
    if (read_runtime_command_line(&line)) {
      handle_runtime_command_line(line);
    }
    return TRUE;
  }
  if (!read_stdin_char(&c))
    return TRUE;
  g_print("\n");

  if (config_selection_active_) {
    config_selection_active_ = false;
    if (c >= '0' && c <= '9') {
      rcfg_ = c - '0';
      if (rcfg_ < app_ctx.size()) {
        g_print("--selecting config  %d--\n", rcfg_);
      } else {
        g_print("--selected config file %d out of bound, reenter\n", rcfg_);
        rcfg_ = 0;
      }
    } else {
      g_print("--config selection cancelled--\n");
    }
    return TRUE;
  }

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
    case '@':
      runtime_command_active_ = true;
      runtime_command_buffer_.clear();
      {
        std::string line;
        if (read_runtime_command_line(&line)) {
          handle_runtime_command_line(line);
        }
      }
      break;
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
        config_selection_active_ = true;
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
  Atom exitEventLoopAtom = XInternAtom(display_, "HM_EXIT_EVENT_LOOP", False);
  disp_lock_.Lock();
  while (display_ && !quit_) {
    XEvent e;
    guint index;
    memset(&e, 0, sizeof(XEvent));
    while (!quit_ && display_ && XPending(display_)) {
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
          if (e.xclient.message_type == exitEventLoopAtom) {
            quit_ = TRUE;
          }
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
    disp_lock_.Unlock();
    g_usleep(G_USEC_PER_SEC / 20);
    disp_lock_.Lock();
  }
  disp_lock_.Unlock();
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
  if (time_limit_seconds_ > 0 && batch_meta &&
      hm::pipeline_internal::stitch_frame_should_account_playback(
          stitch_frame_calibration_active_.load(std::memory_order_acquire))) {
    const uint64_t limit_ns = static_cast<uint64_t>(time_limit_seconds_) * GST_SECOND;
    if (buf) {
      GstClockTime pts = GST_BUFFER_PTS(buf);
      if (GST_CLOCK_TIME_IS_VALID(pts)) {
        const uint64_t pts_ns = static_cast<uint64_t>(pts);
        uint64_t elapsed_ns = 0;
        bool have_elapsed = false;
        {
          std::lock_guard<std::mutex> lock(playback_timing_mu_);
          if (!have_first_pts_) {
            first_pts_ns_ = pts_ns;
            have_first_pts_ = true;
          } else if (pts_ns < first_pts_ns_) {
            first_pts_ns_ = pts_ns;
          } else {
            elapsed_ns = pts_ns - first_pts_ns_;
            have_elapsed = true;
          }
        }
        if (have_elapsed) {
          record_timed_run_progress(elapsed_ns);
          if (elapsed_ns >= limit_ns) {
            if (!quit_) {
              quit_ = TRUE;
              if (main_loop_) {
                g_main_loop_quit(main_loop_);
              }
            }
            return TRUE;
          }
        }
      }
    }

    uint64_t elapsed_from_frames_ns = 0;
    {
      std::lock_guard<std::mutex> lock(playback_timing_mu_);
      for (NvDsMetaList* l_frame = batch_meta->frame_meta_list; l_frame != nullptr; l_frame = l_frame->next) {
        NvDsFrameMeta* frame_meta = reinterpret_cast<NvDsFrameMeta*>(l_frame->data);
        if (!frame_meta || frame_meta->source_id >= MAX_SOURCE_BINS) {
          continue;
        }
        const int source_id = static_cast<int>(frame_meta->source_id);
        const int fps_n = app_ctx->config.multi_source_config[source_id].camera_fps_n;
        const int fps_d = app_ctx->config.multi_source_config[source_id].camera_fps_d;
        if (fps_n <= 0 || fps_d <= 0 || frame_meta->frame_num < 0) {
          continue;
        }
        const uint64_t frame_num = static_cast<uint64_t>(frame_meta->frame_num);
        if (!have_first_frame_by_source_[source_id]) {
          have_first_frame_by_source_[source_id] = true;
          first_frame_numbers_by_source_[source_id] = frame_num;
          continue;
        }
        if (frame_num < first_frame_numbers_by_source_[source_id]) {
          first_frame_numbers_by_source_[source_id] = frame_num;
          continue;
        }
        const uint64_t frame_delta = frame_num - first_frame_numbers_by_source_[source_id];
        const uint64_t elapsed_ns = (frame_delta * static_cast<uint64_t>(GST_SECOND) * static_cast<uint64_t>(fps_d)) /
            static_cast<uint64_t>(fps_n);
        if (elapsed_ns > elapsed_from_frames_ns) {
          elapsed_from_frames_ns = elapsed_ns;
        }
      }
    }
    record_timed_run_progress(elapsed_from_frames_ns);
    if (elapsed_from_frames_ns >= limit_ns) {
      if (!quit_) {
        quit_ = TRUE;
        if (main_loop_) {
          g_main_loop_quit(main_loop_);
        }
      }
    }
  }

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
  ui_preview_channels_.clear();
  destroy_pipeline_for_recreate(app_ctx_ptr);
  g_print("Recreate pipeline\n");
  if (!create_pipeline(app_ctx_ptr, nullptr, all_bbox_generated, perf_cb_static, overlay_graphics_static)) {
    NVGSTDS_ERR_MSG_V("Failed to create pipeline");
    return FALSE;
  }
  auto* hm_app = static_cast<HmApp*>(app_ctx_ptr);
  const uint64_t initial_position_ns = initial_pipeline_position_ns(hm_app);
  const absl::Status position_status =
      hm_app->configurator().prepare_initial_pipeline_position(hm_app->pipeline, hm_app->config, initial_position_ns);
  if (!position_status.ok()) {
    NVGSTDS_ERR_MSG_V("Failed to restore initial pipeline position: %s", position_status.ToString().c_str());
    return FALSE;
  }
  if (!ui_preview_window_ids_.empty()) {
    const auto stage = stage_app_contexts_.find(current_stage_);
    if (stage == stage_app_contexts_.end()) {
      NVGSTDS_ERR_MSG_V("Could not find the current stage while rebuilding GPU preview branches");
      return FALSE;
    }
    const absl::Status preview_status = configure_source_preview_sinks(stage->second);
    if (!preview_status.ok()) {
      NVGSTDS_ERR_MSG_V("Failed to rebuild GPU preview branches: %s", preview_status.ToString().c_str());
      return FALSE;
    }
  }
  if (gst_element_set_state(app_ctx_ptr->pipeline.pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
    NVGSTDS_ERR_MSG_V("Failed to set pipeline to PAUSED");
    return FALSE;
  }
  const absl::Status post_config_status =
      hm_app->configurator().post_config_pipeline(hm_app->pipeline, hm_app->config, initial_position_ns);
  if (!post_config_status.ok()) {
    NVGSTDS_ERR_MSG_V("Failed to restore post-configuration position: %s", post_config_status.ToString().c_str());
    return FALSE;
  }
  for (i = 0; i < app_ctx_ptr->config.num_sink_sub_bins; i++) {
    GstElement* sink = app_ctx_ptr->pipeline.instance_bins[0].sink_bin.sub_bins[i].sink;
    if (!GST_IS_VIDEO_OVERLAY(sink) || manages_its_own_window(sink))
      continue;
    gst_video_overlay_set_window_handle(
        GST_VIDEO_OVERLAY(sink), (gulong)stage_windows_.at(current_stage_)[app_ctx_ptr->index]);
    gst_video_overlay_expose(GST_VIDEO_OVERLAY(sink));
  }
  if (gst_element_set_state(app_ctx_ptr->pipeline.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_print("\ncan't set pipeline to playing state.\n");
    return FALSE;
  }
  return ret;
}

//------------------------------------------------------------------------------
// Main function.
//------------------------------------------------------------------------------
int main(int argc, char* argv[]) {
#if defined(__linux__)
  if (const char* expected_parent_text = std::getenv("HSTREAM_UI_PARENT_PID")) {
    guint64 expected_parent = 0;
    if (!parse_uint64_strict(expected_parent_text, &expected_parent) || expected_parent == 0 ||
        expected_parent > static_cast<guint64>(G_MAXINT) || prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 ||
        getppid() != static_cast<pid_t>(expected_parent)) {
      g_printerr("hstream-cli refused a stale or invalid HSTREAM_UI_PARENT_PID\n");
      return 78;
    }
  }
#endif
  configure_pipeline_runtime_environment(argc > 0 ? argv[0] : nullptr);
  if (!std::getenv("HSTREAM_RUNTIME_ENV_READY")) {
    setenv("HSTREAM_RUNTIME_ENV_READY", "1", 1);
    if (argc > 0 && argv[0]) {
      if (std::strchr(argv[0], '/')) {
        execv(argv[0], argv);
      } else {
        execvp(argv[0], argv);
      }
      std::perror("hstream-cli re-exec failed");
    }
  }
  // Must precede GStreamer sinks or any other Xlib user in this process.
  hm::gpu_preview::initialize_process();
  PipelineApplication app;
  absl::Status status = app.run(argc, argv);
  disable_perf_measurement();
  if (!status.ok()) {
    std::cerr << status << std::endl;
    return status.raw_code();
  }
  return 0;
}
