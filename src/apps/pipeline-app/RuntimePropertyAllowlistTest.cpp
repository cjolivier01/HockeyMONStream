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
          is_allowlisted_runtime_property("playcropper0", "fixed-edge-rotation-angle"),
          "Existing playcropper runtime controls must remain allowlisted") ||
      !expect(
          !is_allowlisted_runtime_property("playcropper0", "plugin-private-config"),
          "Unreviewed playcropper properties must remain blocked") ||
      !expect(
          !is_allowlisted_runtime_property("dsplaytracker0", "shadow-lift"),
          "Shadow lift must remain scoped to playcropper") ||
      !expect(
          !is_allowlisted_runtime_property("untrusted-element", "shadow-lift"),
          "Unknown elements must remain blocked")) {
    return 1;
  }
  return 0;
}
