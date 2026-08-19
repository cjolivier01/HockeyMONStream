#pragma once

#include <functional>
#include <map>
#include <string>

#include <opencv2/core.hpp>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "hstream/src/libs/stitching/RinkSegmentation.h"

namespace hm {
namespace stitching {

/// Mapping from chapter number to filename.
using VideoChapter = std::map<int, std::string>;
/// Videos dictionary mapping a video key (as string) to its chapters.
using VideosDict = std::map<std::string, VideoChapter>;

absl::StatusOr<VideosDict> get_available_videos(const std::string& dir_name, bool prune = false);

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
    const std::function<bool()>& is_cancelled = {});

} // namespace stitching
} // namespace hm
