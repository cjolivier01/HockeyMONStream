#pragma once

#include <charconv>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace hm::pipeline_internal {

struct PreviewOverlaySelection {
  bool players{false};
  bool play{false};
  bool rink{false};

  bool any() const {
    return players || play || rink;
  }

  bool operator==(const PreviewOverlaySelection& other) const {
    return players == other.players && play == other.play && rink == other.rink;
  }
};

struct PreviewOverlayCommand {
  uint64_t generation{0};
  PreviewOverlaySelection selection;
};

inline bool preview_overlay_channel_supports_diagnostics(std::string_view channel) {
  return channel == "program" || channel == "stitched";
}

inline std::string preview_channel_for_pipeline_start(
    std::string_view active_channel,
    std::string_view initial_channel,
    bool explicitly_disabled) {
  if (explicitly_disabled)
    return "none";
  return std::string(active_channel.empty() ? initial_channel : active_channel);
}

inline bool is_preview_overlay_command(std::string_view line) {
  std::istringstream input{std::string(line)};
  std::string verb;
  return (input >> verb) && verb == "set-preview-overlays";
}

inline bool parse_preview_overlay_command(std::string_view line, PreviewOverlayCommand* command) {
  if (!command)
    return false;
  std::istringstream input{std::string(line)};
  std::string verb;
  std::string generation_text;
  std::string players_text;
  std::string play_text;
  std::string rink_text;
  std::string extra;
  if (!(input >> verb >> generation_text >> players_text >> play_text >> rink_text) || input >> extra ||
      verb != "set-preview-overlays") {
    return false;
  }
  uint64_t generation = 0;
  const char* first = generation_text.data();
  const char* last = first + generation_text.size();
  const auto parsed = std::from_chars(first, last, generation);
  if (generation_text.empty() || parsed.ec != std::errc() || parsed.ptr != last || generation == 0)
    return false;
  auto parse_flag = [](const std::string& value, bool* out) {
    if (value != "0" && value != "1")
      return false;
    *out = value == "1";
    return true;
  };
  PreviewOverlayCommand result;
  result.generation = generation;
  if (!parse_flag(players_text, &result.selection.players) || !parse_flag(play_text, &result.selection.play) ||
      !parse_flag(rink_text, &result.selection.rink)) {
    return false;
  }
  *command = result;
  return true;
}

class PreviewOverlayRuntimeState {
 public:
  explicit PreviewOverlayRuntimeState(PreviewOverlaySelection selection = {}) : selection_(selection) {}

  uint64_t generation() const {
    return generation_;
  }

  const PreviewOverlaySelection& selection() const {
    return selection_;
  }

  bool is_fresh(const PreviewOverlayCommand& command) const {
    return command.generation > generation_;
  }

  void commit(const PreviewOverlayCommand& command) {
    generation_ = command.generation;
    selection_ = command.selection;
  }

 private:
  uint64_t generation_{0};
  PreviewOverlaySelection selection_;
};

} // namespace hm::pipeline_internal
