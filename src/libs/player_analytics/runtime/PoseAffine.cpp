#include "hstream/src/libs/player_analytics/runtime/PoseGpu.h"

#include <algorithm>
#include <cmath>

namespace hm::player_analytics {

absl::StatusOr<PoseAffine> MakePoseAffine(const ImageView& image, const PoseRoi& roi) {
  const auto& box = roi.metadata_box;
  for (float value : {box.left, box.top, box.width, box.height, roi.metadata_width, roi.metadata_height})
    if (!std::isfinite(value) || std::abs(value) > 1000000)
      return absl::InvalidArgumentError("pose ROI coordinates must be finite and bounded metadata pixels");
  if (image.width < 1 || image.height < 1 || image.width > 65536 || image.height > 65536 || box.width <= 0 ||
      box.height <= 0 || roi.metadata_width <= 0 || roi.metadata_height <= 0)
    return absl::InvalidArgumentError("pose ROI and surface dimensions must be positive and bounded");
  const float scale_x = image.width / roi.metadata_width;
  const float scale_y = image.height / roi.metadata_height;
  const float center_x = (box.left + box.width * 0.5F) * scale_x;
  const float center_y = (box.top + box.height * 0.5F) * scale_y;
  float width = box.width * scale_x * 1.25F;
  float height = box.height * scale_y * 1.25F;
  if (width > height * 0.75F)
    height = width / 0.75F;
  else
    width = height * 0.75F;
  if (!std::isfinite(center_x) || !std::isfinite(center_y) || !std::isfinite(width) || !std::isfinite(height) ||
      std::abs(center_x) > 1000000 || std::abs(center_y) > 1000000 || width < 0.001F || height < 0.001F ||
      width > 1000000 || height > 1000000)
    return absl::InvalidArgumentError("pose ROI expansion exceeds bounded surface coordinates");

  // MMPose constructs float32 source control points, solves their affine in
  // double, then casts the forward matrix to float32 before cv::warpAffine.
  const float left = center_x - width * 0.5F;
  const double half_width = static_cast<double>(center_x) - left;
  if (half_width <= 0)
    return absl::InvalidArgumentError("pose ROI is too small at its coordinate magnitude");
  const float bottom = center_y + static_cast<float>(half_width);
  const double half_height = static_cast<double>(bottom) - center_y;
  if (half_height <= 0)
    return absl::InvalidArgumentError("pose ROI is too small at its vertical coordinate magnitude");
  const double exact_scale = (kPoseInputWidth * 0.5) / half_width;
  const double exact_scale_y = (kPoseInputWidth * 0.5) / half_height;
  const float forward_scale = static_cast<float>(exact_scale);
  const float forward_scale_y = static_cast<float>(exact_scale_y);
  const float forward_x = static_cast<float>(kPoseInputWidth * 0.5 - center_x * exact_scale);
  const float forward_y = static_cast<float>(kPoseInputHeight * 0.5 - center_y * exact_scale_y);
  PoseAffine result;
  result.dx = 1.0 / forward_scale;
  result.dy = 1.0 / forward_scale_y;
  result.x0 = -forward_x / static_cast<double>(forward_scale);
  result.y0 = -forward_y / static_cast<double>(forward_scale_y);
  result.surface_to_metadata_x = 1 / scale_x;
  result.surface_to_metadata_y = 1 / scale_y;
  return result;
}

} // namespace hm::player_analytics
