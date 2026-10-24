#pragma once

#include <cstddef>
#include "hockeymon/csrc/play_tracker/BoxUtils.h"

namespace hm::playcropper {
struct FrameTransformGeometry {
  BBox source_rect;
  Point anchor_point;
  BBox crop_box;
  float angle;
};

FrameTransformGeometry CalculateFrameTransformGeometry(
    size_t input_width,
    size_t input_height,
    const BBox& tracking_box,
    bool no_crop,
    float fixed_edge_rotation_angle_left,
    float fixed_edge_rotation_angle_right);

} // namespace hm::playcropper
