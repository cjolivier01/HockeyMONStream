#include "hstream/src/libs/program/FrameGeometry.h"

namespace hm::playcropper {
absl::StatusOr<BBox> ResolveTrackingBox(
    size_t input_width,
    size_t input_height,
    size_t metadata_width,
    size_t metadata_height,
    size_t output_width,
    size_t output_height,
    const std::optional<BBox>& tracking_box) {
  if (!input_width || !input_height || !metadata_width || !metadata_height || !output_width || !output_height)
    return absl::InvalidArgumentError("Program crop dimensions must be positive");
  if (tracking_box)
    return tracking_box->make_canvas_scaled(
        FloatValue(input_width) / metadata_width, FloatValue(input_height) / metadata_height);

  const double output_aspect = double(output_width) / output_height;
  const double width = std::min(double(input_width), input_height * output_aspect);
  const double height = width / output_aspect;
  return BBox(0, 0, width, height).at_center(BBox(0, 0, input_width, input_height).center());
}

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
