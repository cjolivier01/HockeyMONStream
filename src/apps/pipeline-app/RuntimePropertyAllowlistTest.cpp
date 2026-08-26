#include "RuntimePropertyAllowlist.h"

#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

} // namespace

int main() {
  using hm::pipeline::is_allowlisted_runtime_property;
  if (!expect(
          is_allowlisted_runtime_property("playcropper0", "shadow-lift"),
          "The production playcropper instance must accept live shadow lift") ||
      !expect(
          is_allowlisted_runtime_property("playcropper", "shadow-lift"),
          "The playcropper compatibility alias must accept live shadow lift") ||
      !expect(
          is_allowlisted_runtime_property("playcropper0", "shadow-lift-black-point"),
          "The production playcropper instance must accept live black-point lift") ||
      !expect(
          is_allowlisted_runtime_property("playcropper", "shadow-lift-black-point"),
          "The playcropper compatibility alias must accept live black-point lift") ||
      !expect(
          is_allowlisted_runtime_property("playcropper0", "exposure") &&
              is_allowlisted_runtime_property("playcropper", "exposure"),
          "Both playcropper names must accept live exposure") ||
      !expect(
          is_allowlisted_runtime_property("hmstitcher0", "shadow-lift") &&
              is_allowlisted_runtime_property("hmstitcher0", "shadow-lift-black-point") &&
              is_allowlisted_runtime_property("hmstitcher0", "exposure"),
          "The FP16 stitcher must accept all live grading controls") ||
      !expect(
          !is_allowlisted_runtime_property("hmstitcher0", "high-bit-depth"),
          "High-bit mode must remain restart-only") ||
      !expect(
          is_allowlisted_runtime_property("playcropper0", "fixed-edge-rotation-angle"),
          "Existing playcropper runtime controls must remain allowlisted") ||
      !expect(
          is_allowlisted_runtime_property("playcropper0", "scoreboard-perspective-polygon"),
          "Live geometry changes must be able to disable a stale scoreboard polygon") ||
      !expect(
          !is_allowlisted_runtime_property("playcropper0", "plugin-private-config"),
          "Unreviewed playcropper properties must remain blocked") ||
      !expect(
          !is_allowlisted_runtime_property("dsplaytracker0", "shadow-lift"),
          "Shadow lift must remain scoped to playcropper") ||
      !expect(
          !is_allowlisted_runtime_property("dsplaytracker0", "shadow-lift-black-point"),
          "Black-point lift must remain scoped to playcropper") ||
      !expect(
          !is_allowlisted_runtime_property("dsplaytracker0", "exposure"),
          "Exposure must remain scoped to playcropper") ||
      !expect(
          !is_allowlisted_runtime_property("untrusted-element", "shadow-lift"),
          "Unknown elements must remain blocked") ||
      !expect(
          is_allowlisted_runtime_property("dsplaytracker0", "draw"),
          "The play-tracker display-meta producer must be live-toggleable") ||
      !expect(
          is_allowlisted_runtime_property("program_gpu_preview_sink", "show-player-tracking") &&
              is_allowlisted_runtime_property("stitched_gpu_preview_sink", "show-play-tracking") &&
              is_allowlisted_runtime_property("hmstitcher_preview_sink", "show-rink-mask"),
          "GPU preview debug layers must be live-toggleable on both preview channels") ||
      !expect(
          !is_allowlisted_runtime_property("program_gpu_preview_sink", "rink-mask-file"),
          "Runtime commands must not replace the trusted game-scoped rink-mask path")) {
    return 1;
  }
  return 0;
}
