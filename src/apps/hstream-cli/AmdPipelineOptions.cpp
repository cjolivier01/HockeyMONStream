#include "AmdPipelineOptions.h"

#include <glib.h>

#include <sstream>
#include <stdexcept>

#include "hstream/src/libs/common/utils.h"

namespace hm::program {
namespace {
struct RawOptions {
  gchar *game{}, *sources{}, *sinks{}, *start{}, *limit{};
  gchar **applications{}, **options{};
  gboolean realtime{false}, show{false};
  gint64 window{};
  ~RawOptions() {
    for (auto* value : {game, sources, sinks, start, limit})
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
        {"enable-sinks", 0, 0, G_OPTION_ARG_STRING, &raw.sinks, "Program outputs", "FAKE,RENDER,ENCODE_FILE"},
        {"options", 'o', 0, G_OPTION_ARG_STRING_ARRAY, &raw.options, "Configuration overrides", "KEY=VALUE,..."},
        {"start-time", 's', 0, G_OPTION_ARG_STRING, &raw.start, "Synchronized start time", "SECONDS|HH:MM:SS"},
        {"time-limit", 't', 0, G_OPTION_ARG_STRING, &raw.limit, "Completed video duration", "SECONDS|HH:MM:SS"},
        {"render-realtime", 0, 0, G_OPTION_ARG_NONE, &raw.realtime, "Pace preview to source time", nullptr},
        {"render-window-id", 0, 0, G_OPTION_ARG_INT64, &raw.window, "Native preview window", "XID"},
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
      Require(name == "FAKE" || name == "RENDER" || name == "ENCODE_FILE", "Unsupported AMD Program sink: " + name);
      Require(result.sinks.insert(name).second, "Duplicate Program sink: " + name);
    }
    Require(
        (!raw.show && !raw.window && !raw.realtime) || result.sinks.count("RENDER"),
        "Preview options require the RENDER sink");
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
