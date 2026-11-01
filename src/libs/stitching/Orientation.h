#pragma once

#include <functional>
#include <string>

#include <opencv2/core.hpp>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "hstream/src/libs/stitching/CameraSourceStart.h"
#include "hstream/src/libs/stitching/RinkSegmentation.h"
#include "hstream/src/libs/stitching/VideoDiscovery.h"

namespace hm {
namespace stitching {

struct OrientationScores {
  double left{0.0};
  double right{0.0};
  double top{0.0};
  double bottom{0.0};
};

absl::StatusOr<OrientationScores> rink_orientation_scores(const cv::Mat& binary_mask);
absl::StatusOr<std::string> classify_rink_orientation(const cv::Mat& binary_mask);
absl::Status configure_game_orientation(
    const std::string& game_dir,
    const RinkSegmentation& rink_model,
    const std::string& expected_invalidation_id = {},
    const std::function<bool()>& is_cancelled = {},
    const CameraSourceStarts& source_starts = {});

} // namespace stitching
} // namespace hm
