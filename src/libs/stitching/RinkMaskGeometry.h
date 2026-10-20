#pragma once

#include <functional>
#include <optional>
#include <vector>

#include <opencv2/core.hpp>
#include "absl/status/statusor.h"

namespace hm::stitching {
struct RinkMaskBoard {
  std::vector<cv::Vec3d> rays;
  double rms_degrees{0};
};

// Offline, bounded proposal from the upper, central ice silhouette.
// The caller supplies rays for normalized mask pixel centres in the matching
// calibrated output geometry. Missing/invalid rays break boundary support.
absl::StatusOr<RinkMaskBoard> SuggestFarRinkBoard(
    const cv::Mat& mask,
    const std::function<std::optional<cv::Vec3d>(double, double)>& ray_at);

// Smooth starting arc from the lower, central silhouette. Fits a straight
// board's viewing-ray plane robustly, bridging local occlusions/door openings
// instead of following them. Returned rays sample that smooth arc. The mask
// may follow board tops: the operator must adjust/review this at ice level.
absl::StatusOr<RinkMaskBoard> SuggestNearRinkBoard(
    const cv::Mat& mask,
    const std::function<std::optional<cv::Vec3d>(double, double)>& ray_at);
} // namespace hm::stitching
