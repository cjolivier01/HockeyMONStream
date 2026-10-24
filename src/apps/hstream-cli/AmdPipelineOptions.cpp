#include "AmdPipelineOptions.h"

#include <glib.h>

#include <charconv>
#include <sstream>
#include <stdexcept>

#include "hstream/src/libs/common/utils.h"

namespace hm::program {
namespace {
struct RawOptions {
  gchar *game{}, *sources{}, *sinks{}, *start{}, *limit{}, *preview_windows{}, *preview_active{};
  gchar *stitch_time{}, *invalidation{};
  gchar **applications{}, **options{};
  gboolean realtime{false}, show{false};
  gint64 window{};
  ~RawOptions() {
    for (auto* value : {game, sources, sinks, start, limit, preview_windows, preview_active, stitch_time, invalidation})
      g_free(value);
    g_strfreev(applications);
    g_strfreev(options);
  }
};
void Require(bool ok, const std::string& detail) {
  if (!ok)
    throw std::invalid_argument(detail);
}
uint64_t Time(const char* text) {
  const auto time = hhmmss_to_nanoseconds(text);
  Require(time <= uint64_t(INT64_MAX), "Time exceeds the signed video clock");
  return time;
}
} // namespace
absl::StatusOr<AmdPipelineOptions> ParseAmdPipelineOptions(std::vector<std::string> arguments) {
  try {
    Require(!arguments.empty(), "Missing executable name");
    std::vector<char*> pointers;
    for (auto& argument : arguments) {
      if (argument.rfind("-t=", 0) == 0)
        argument = "--time-limit=" + argument.substr(3);
      pointers.push_back(argument.data());
    }
    pointers.push_back(nullptr);
    auto* argv = pointers.data();
    int argc = arguments.size();
    RawOptions raw;
    GOptionEntry entries[] = {
        {"game-id", 'g', 0, G_OPTION_ARG_STRING, &raw.game, "Saved calibrated game", "ID"},
        {"cfg-file", 'c', 0, G_OPTION_ARG_FILENAME_ARRAY, &raw.applications, "One application YAML", "FILE"},
        {"enable-sources", 0, 0, G_OPTION_ARG_STRING, &raw.sources, "Saved camera chapters", "URI-MULTIPLE"},
        {"enable-sinks",
         0,
         0,
         G_OPTION_ARG_STRING,
         &raw.sinks,
         "Program outputs",
         "FAKE,RENDER,ENCODE_FILE,ENCODE_PROGRAM_4K_FILE"},
        {"options", 'o', 0, G_OPTION_ARG_STRING_ARRAY, &raw.options, "Configuration overrides", "KEY=VALUE,..."},
        {"start-time", 's', 0, G_OPTION_ARG_STRING, &raw.start, "Synchronized start time", "SECONDS|HH:MM:SS"},
        {"time-limit", 't', 0, G_OPTION_ARG_STRING, &raw.limit, "Completed video duration", "SECONDS|HH:MM:SS"},
        {"render-realtime", 0, 0, G_OPTION_ARG_NONE, &raw.realtime, "Pace preview to source time", nullptr},
        {"render-window-id", 0, 0, G_OPTION_ARG_INT64, &raw.window, "Native preview window", "XID"},
        {"stitch-frame-time", 0, 0, G_OPTION_ARG_STRING, &raw.stitch_time, "Saved calibration reference", "TIME"},
        {"clean-expected-invalidation-id",
         0,
         0,
         G_OPTION_ARG_STRING,
         &raw.invalidation,
         "Expected saved calibration revision",
         "ID"},
        {"ui-preview-windows",
         0,
         0,
         G_OPTION_ARG_STRING,
         &raw.preview_windows,
         "Desktop preview windows",
         "CHANNEL:XID,..."},
        {"ui-preview-active",
         0,
         0,
         G_OPTION_ARG_STRING,
         &raw.preview_active,
         "Initial desktop preview",
         "CHANNEL|none"},
        {"show", 0, 0, G_OPTION_ARG_NONE, &raw.show, "Show the selected render output", nullptr},
        {nullptr}};
    auto* context = g_option_context_new("— native AMD Program playback");
    g_option_context_add_main_entries(context, entries, nullptr);
    GError* error{};
    const bool parsed = g_option_context_parse(context, &argc, &argv, &error);
    const std::string detail = error ? error->message : "";
    g_clear_error(&error);
    g_option_context_free(context);
    Require(parsed && argc == 1, detail.empty() ? "Unexpected positional argument" : detail);
    Require(raw.game && *raw.game && raw.sinks && *raw.sinks, "Program requires --game-id and --enable-sinks");
    Require(
        !raw.applications || !raw.applications[1],
        "AMD Program accepts one application YAML; two-stage calibration is not supported");
    Require(
        !raw.sources || std::string(raw.sources) == "URI-MULTIPLE", "AMD Program requires URI-MULTIPLE saved cameras");
    Require(raw.window >= 0 && uint64_t(raw.window) <= UINTPTR_MAX, "Invalid native window ID");
    AmdPipelineOptions result;
    result.game = raw.game;
    if (raw.stitch_time) {
      Require(*raw.stitch_time, "Empty saved calibration reference");
      result.saved_stitch_time = raw.stitch_time;
    }
    if (raw.invalidation) {
      Require(*raw.invalidation, "Empty saved calibration revision");
      result.expected_invalidation = raw.invalidation;
    }
    if (raw.applications) {
      Require(*raw.applications[0], "Empty application YAML path");
      result.application = raw.applications[0];
    }
    if (raw.options)
      for (auto** option = raw.options; *option; ++option)
        result.options.emplace_back(*option);
    std::istringstream names(raw.sinks);
    Require(std::string(raw.sinks).back() != ',', "Empty Program sink");
    for (std::string name; std::getline(names, name, ',');) {
      Require(
          name == "FAKE" || name == "RENDER" || name == "ENCODE_FILE" || name == "ENCODE_PROGRAM_4K_FILE",
          "Unsupported AMD Program sink: " + name);
      Require(result.sinks.insert(name).second, "Duplicate Program sink: " + name);
    }
    Require(
        (!raw.show && !raw.window && !raw.realtime && !raw.preview_windows && !raw.preview_active) ||
            result.sinks.count("RENDER"),
        "Preview options require the RENDER sink");
    if (raw.preview_windows || raw.preview_active) {
      Require(
          raw.preview_windows && *raw.preview_windows && raw.preview_active && *raw.preview_active,
          "Desktop preview requires both windows and an initial channel");
      Require(!raw.window, "Desktop preview windows cannot accompany --render-window-id");
      Require(std::string(raw.preview_windows).back() != ',', "Empty desktop preview target");
      std::set<uintptr_t> ids;
      std::istringstream windows(raw.preview_windows);
      for (std::string part; std::getline(windows, part, ',');) {
        const auto colon = part.find(':');
        Require(colon != std::string::npos, "Preview target must be CHANNEL:XID");
        const auto channel = part.substr(0, colon), id = part.substr(colon + 1);
        Require(
            channel == "program" || channel == "stitched" || channel == "source0" || channel == "source1" ||
                channel == "source2",
            "Unknown AMD desktop preview channel");
        uintptr_t window = 0;
        const auto parsed = std::from_chars(id.data(), id.data() + id.size(), window);
        Require(parsed.ec == std::errc{} && parsed.ptr == id.data() + id.size() && window, "Invalid desktop window ID");
        Require(
            result.preview_windows.emplace(channel, window).second && ids.insert(window).second,
            "Duplicate desktop preview target");
      }
      Require(result.preview_windows.count("program"), "AMD desktop preview requires a Program window");
      result.preview_active = raw.preview_active;
      Require(
          result.preview_active == "none" || result.preview_windows.count(result.preview_active),
          "Initial preview channel must have a window");
    }
    if (raw.start)
      result.start = Time(raw.start);
    if (raw.limit) {
      result.limit = Time(raw.limit);
      Require(*result.limit > 0, "Time limit must be positive");
    }
    result.realtime = raw.realtime;
    result.window = raw.window;
    return result;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  }
}
} // namespace hm::program
