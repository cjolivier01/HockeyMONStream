#include "PreviewOverlayRuntime.h"

#include <iostream>
#include <limits>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

} // namespace

int main() {
  using hm::pipeline_internal::is_preview_overlay_command;
  using hm::pipeline_internal::parse_preview_overlay_command;
  using hm::pipeline_internal::preview_channel_for_pipeline_start;
  using hm::pipeline_internal::preview_overlay_channel_supports_diagnostics;
  using hm::pipeline_internal::PreviewOverlayCommand;
  using hm::pipeline_internal::PreviewOverlayRuntimeState;
  using hm::pipeline_internal::PreviewOverlaySelection;

  bool ok = true;
  PreviewOverlayCommand command;
  ok &= expect(
      parse_preview_overlay_command("set-preview-overlays 17 1 0 1", &command) && command.generation == 17 &&
          command.selection == PreviewOverlaySelection{true, false, true},
      "valid preview-overlay command must parse");
  ok &= expect(
      parse_preview_overlay_command(" \tset-preview-overlays\t18 0\t1 0  \n", &command) &&
          command.selection == PreviewOverlaySelection{false, true, false},
      "ASCII whitespace between exact command fields must be accepted");
  ok &= expect(
      parse_preview_overlay_command(
          "set-preview-overlays " + std::to_string(std::numeric_limits<uint64_t>::max()) + " 1 1 1", &command) &&
          command.generation == std::numeric_limits<uint64_t>::max(),
      "maximum generation must parse without truncation");

  for (const std::string malformed : {
           "",
           "set-preview-overlays",
           "set-preview-overlays 0 1 1 1",
           "set-preview-overlays -1 1 1 1",
           "set-preview-overlays +1 1 1 1",
           "set-preview-overlays 1 2 0 0",
           "set-preview-overlays 1 true 0 0",
           "set-preview-overlays 1 1 0",
           "set-preview-overlays 1 1 0 1 extra",
           "set-preview-overlaysx 1 1 0 1",
           "set-preview-overlays 18446744073709551616 1 0 1",
       }) {
    ok &= expect(!parse_preview_overlay_command(malformed, &command), "malformed command must be rejected");
  }
  ok &= expect(
      is_preview_overlay_command(" set-preview-overlays nonsense") &&
          !is_preview_overlay_command("set-preview-overlaysx 1 1 1 1"),
      "dispatch must recognize only the exact preview-overlay verb");
  ok &= expect(
      preview_overlay_channel_supports_diagnostics("program") &&
          preview_overlay_channel_supports_diagnostics("stitched") &&
          !preview_overlay_channel_supports_diagnostics("source0") &&
          !preview_overlay_channel_supports_diagnostics("none"),
      "overlay selection must target Program/Stitched sinks, never camera previews");
  ok &= expect(
      preview_channel_for_pipeline_start("", "program", true) == "none" &&
          preview_channel_for_pipeline_start("", "program", false) == "program" &&
          preview_channel_for_pipeline_start("stitched", "program", false) == "stitched",
      "an explicit none selection must survive later pipeline stages without being confused with an unset channel");

  PreviewOverlayRuntimeState state(PreviewOverlaySelection{true, false, false});
  const PreviewOverlayCommand first{1, {false, true, true}};
  ok &= expect(state.generation() == 0 && state.is_fresh(first), "generation one must be accepted initially");
  state.commit(first);
  ok &= expect(
      state.generation() == 1 && state.selection() == first.selection && !state.is_fresh(first) &&
          !state.is_fresh(PreviewOverlayCommand{0, {true, true, true}}) &&
          state.is_fresh(PreviewOverlayCommand{2, {true, false, false}}),
      "committed generations must fence duplicate and stale commands");
  return ok ? 0 : 1;
}
