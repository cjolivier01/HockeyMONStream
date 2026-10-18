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

// Offline, bounded proposal from the upper, central ice silhouette. The lower
// silhouette is NEVER evidence for an ice-level board: it often follows the
// top of the near boards, glass or posts. A proposal still requires review.
// The caller supplies rays for normalized mask pixel centres in the matching
// calibrated output geometry. Missing/invalid rays break boundary support.
absl::StatusOr<RinkMaskBoard> SuggestFarRinkBoard(
    const cv::Mat& mask,
    const std::function<std::optional<cv::Vec3d>(double, double)>& ray_at);
} // namespace hm::stitching
