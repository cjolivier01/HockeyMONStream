#include "hstream/src/libs/program/FrameGeometry.h"

namespace hm::playcropper {
FrameTransformGeometry CalculateFrameTransformGeometry(
    size_t input_width,
    size_t input_height,
    const BBox& tracking_box,
    bool no_crop,
    float fixed_edge_rotation_angle_left,
    float fixed_edge_rotation_angle_right) {
  const BBox input_rect(0, 0, input_width, input_height);
  const float half_width = static_cast<float>(input_width) / 2.0f;
  const float tracking_center_x = tracking_box.center().x;
  float angle = 0.0f;
  if (half_width > 0.0f && tracking_center_x < half_width) {
    const float pct = 1.0f - tracking_center_x / half_width;
    angle = fixed_edge_rotation_angle_left * pct;
  } else if (half_width > 0.0f && tracking_center_x > half_width) {
    const float pct = (half_width - tracking_center_x) / half_width;
    angle = fixed_edge_rotation_angle_right * pct;
  }

  if (no_crop) {
    // HockeyMOM applies perspective rotation before its final no-crop frame
    // transform. Keep the camera box as the rotation anchor while selecting
    // the full rotated input for output.
    return FrameTransformGeometry{input_rect, tracking_box.center(), input_rect, angle};
  }

  const FloatValue minimum_width_per_side = tracking_box.width() / 2;
  const FloatValue clip_left = std::max(input_rect.left, tracking_center_x - minimum_width_per_side);
  const FloatValue clip_right = std::min(input_rect.right, tracking_center_x + minimum_width_per_side);
  const BBox source_rect(clip_left, input_rect.top, clip_right, input_rect.bottom);
  BBox local_tracking_box = tracking_box;
  local_tracking_box.left -= source_rect.left;
  local_tracking_box.right -= source_rect.left;
  return FrameTransformGeometry{source_rect, local_tracking_box.center(), local_tracking_box, angle};
}

} // namespace hm::playcropper
