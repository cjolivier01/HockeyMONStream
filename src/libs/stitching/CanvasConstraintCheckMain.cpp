#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"

#include <charconv>
#include <cstddef>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc != 3 && argc != 4) {
    std::cerr << "usage: stitching-canvas-check GAME_DIR MAX_OUTPUT_WIDTH [--hold-lock]\n";
    return 2;
  }
  const bool hold_lock = argc == 4 && std::string(argv[3]) == "--hold-lock";
  if (argc == 4 && !hold_lock) {
    std::cerr << "unknown stitching-canvas-check option\n";
    return 2;
  }
  const std::string width_text = argv[2];
  size_t max_output_width = 0;
  const auto parsed = std::from_chars(width_text.data(), width_text.data() + width_text.size(), max_output_width);
  if (parsed.ec != std::errc() || parsed.ptr != width_text.data() + width_text.size()) {
    std::cerr << "maximum output width must be a non-negative integer\n";
    return 2;
  }
  auto check = hm::stitching::try_lock_canvas_constraint_check(argv[1], max_output_width);
  if (!check.ok()) {
    std::cerr << check.status() << '\n';
    return 1;
  }
  std::cout << "HSTREAM_STITCHING_CANVAS_CHECK artifacts-compatible=" << (check->artifacts_compatible ? 1 : 0)
            << " requires-regeneration=" << (check->requires_regeneration ? 1 : 0)
            << " lock-held=" << (check->artifact_lock ? 1 : 0) << std::endl;
  if (hold_lock && check->artifact_lock) {
    for (char byte = 0; std::cin.get(byte);) {
    }
  }
  return 0;
}
