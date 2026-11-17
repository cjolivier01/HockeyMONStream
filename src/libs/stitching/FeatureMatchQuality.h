#pragma once

#include <cstddef>
#include <vector>

#include <opencv2/core.hpp>
#include <yaml-cpp/yaml.h>

#include "absl/status/statusor.h"
#include "hstream/src/libs/stitching/ControlPointMatcher.h"

namespace hm::stitching {

// Required deployment policy, resolved from the bundled baseline and frozen
// when the matcher is created. Coverage never relaxes these quality bounds.
struct FeatureMatchQuality {
  float akaze_response_floor;
  float akaze_max_hamming_fraction;
  float superpoint_min_score;
  float dedode_min_score;
  float loftr_min_score;
  float MinimumScore(ControlPointMatcher matcher) const;
};

absl::StatusOr<FeatureMatchQuality> ReadFeatureMatchQuality(const YAML::Node& baseline);
absl::StatusOr<FeatureMatchQuality> LoadFeatureMatchQuality();

// Score-ordered suppression with an adaptive radius, bounded by the detector
// budget. Weak features are removed before searching for spatial coverage.
absl::StatusOr<std::vector<cv::KeyPoint>> SelectDistributedKeypoints(
    std::vector<cv::KeyPoint> keypoints,
    cv::Size image_size,
    size_t maximum,
    float response_floor);

// Ratio + mutual checks plus an absolute descriptor-distance ceiling. Requires
// two neighbors in each image: an absent runner-up cannot establish uniqueness.
absl::StatusOr<std::vector<cv::DMatch>> MatchQualityBinaryDescriptors(
    const cv::Mat& left,
    const cv::Mat& right,
    float lowe_ratio,
    float maximum_hamming_fraction);

} // namespace hm::stitching
