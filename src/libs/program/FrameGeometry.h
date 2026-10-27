#pragma once

#include <cstddef>
#include <optional>
#include "absl/status/statusor.h"
#include "hockeymon/csrc/play_tracker/BoxUtils.h"

namespace hm::playcropper {
struct FrameTransformGeometry {
  BBox source_rect;
  Point anchor_point;
  BBox crop_box;
  float angle;
};

// A recorded follower needs metadata-to-surface scaling. A missing follower
// instead uses the largest centered output-aspect crop in surface coordinates.
absl::StatusOr<BBox> ResolveTrackingBox(
    size_t input_width,
    size_t input_height,
    size_t metadata_width,
    size_t metadata_height,
    size_t output_width,
    size_t output_height,
    const std::optional<BBox>& tracking_box);

FrameTransformGeometry CalculateFrameTransformGeometry(
    size_t input_width,
    size_t input_height,
    const BBox& tracking_box,
    bool no_crop,
    float fixed_edge_rotation_angle_left,
    float fixed_edge_rotation_angle_right);

} // namespace hm::playcropper
