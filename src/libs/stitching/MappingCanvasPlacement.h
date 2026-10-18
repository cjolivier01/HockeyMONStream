#pragma once

#include <cstddef>
#include <filesystem>

#include "absl/status/statusor.h"

namespace hm::stitching {
// Published NONA remap union in uncropped PTO output coordinates. The live
// stitcher normalizes both cameras by this origin before composing the canvas.
struct MappingCanvasPlacement {
  double left{0}, top{0};
  size_t width{0}, height{0};
};
// Reads only TIFF placement headers. Caller holds the artifact lock and has
// validated the generation's bounded regular files before calling.
absl::StatusOr<MappingCanvasPlacement> read_mapping_canvas_placement_locked(const std::filesystem::path& game_dir);
} // namespace hm::stitching
