#pragma once

#include <array>

#include "absl/status/statusor.h"
#include "hstream/src/libs/gpu/Image.h"

namespace hm::scoreboard {
// A post-decode crop, not a claim that the codec can decode only this region.
// Keep the original polygon for provenance; use the translated polygon when
// sampling the cropped GPU image. Bounds include every bilinear sampling tap.
struct CameraRegion {
  int left, top, width, height;
  std::array<std::array<float, 2>, 4> polygon;
};

// alignment is the backend's pixel/chroma requirement, supplied explicitly.
// Frames whose dimensions cannot satisfy it must use a full-frame path.
absl::StatusOr<CameraRegion> MakeCameraRegion(
    const std::array<std::array<float, 2>, 4>& polygon,
    int source_width,
    int source_height,
    int alignment);

// Borrow a pitch-linear GPU subimage without copying or mapping its pixels.
// The original image owner must outlive every consumer. YUV views require
// chroma-aligned bounds; packed formats may use any integral pixel bounds.
absl::StatusOr<gpu::Image> CropCameraImage(const gpu::Image& source, const CameraRegion& region);
} // namespace hm::scoreboard
