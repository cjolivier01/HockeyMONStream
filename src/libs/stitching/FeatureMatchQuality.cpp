#include "hstream/src/libs/stitching/FeatureMatchQuality.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <opencv2/features2d.hpp>

#include "absl/status/status.h"
#include "hstream/src/libs/common/BaselineConfig.h"

namespace hm::stitching {

float FeatureMatchQuality::MinimumScore(ControlPointMatcher matcher) const {
  switch (matcher) {
    case ControlPointMatcher::kSuperPointLightGlue:
      return superpoint_min_score;
    case ControlPointMatcher::kDeDoDeLightGlue:
      return dedode_min_score;
    case ControlPointMatcher::kLoFTR:
      return loftr_min_score;
    case ControlPointMatcher::kAkazeHamming:
      return 1.0f - akaze_max_hamming_fraction;
  }
  return 1.0f;
}

absl::StatusOr<FeatureMatchQuality> ReadFeatureMatchQuality(const YAML::Node& baseline) {
  try {
    const auto node = baseline["stitching"]["match_quality"];
    if (!node.IsMap())
      return absl::InvalidArgumentError("stitching.match_quality is required in the baseline");
    const auto number = [&](const char* key) {
      const float value = node[key].as<float>();
      if (!std::isfinite(value) || value <= 0.0f || value >= 1.0f)
        throw std::runtime_error(std::string("Expected a finite value between zero and one for ") + key);
      return value;
    };
    return FeatureMatchQuality{
        number("akaze_response_floor"),
        number("akaze_max_hamming_fraction"),
        number("superpoint_min_score"),
        number("dedode_min_score"),
        number("loftr_min_score")};
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid stitching.match_quality: ") + error.what());
  }
}

absl::StatusOr<FeatureMatchQuality> LoadFeatureMatchQuality() {
  const auto baseline = hm::baseline_config::load();
  if (!baseline.ok())
    return baseline.status();
  return ReadFeatureMatchQuality(baseline->values);
}

absl::StatusOr<std::vector<cv::KeyPoint>> SelectDistributedKeypoints(
    std::vector<cv::KeyPoint> keypoints,
    cv::Size image_size,
    size_t maximum,
    float response_floor) {
  if (image_size.width <= 0 || image_size.height <= 0 || maximum == 0 || !std::isfinite(response_floor) ||
      response_floor <= 0)
    return absl::InvalidArgumentError("Invalid distributed keypoint selection bounds");
  for (const auto& point : keypoints) {
    if (!std::isfinite(point.response) || !std::isfinite(point.pt.x) || !std::isfinite(point.pt.y) || point.pt.x < 0 ||
        point.pt.y < 0 || point.pt.x >= image_size.width || point.pt.y >= image_size.height)
      return absl::InvalidArgumentError("Invalid detector keypoint");
  }
  keypoints.erase(
      std::remove_if(
          keypoints.begin(), keypoints.end(), [&](const auto& point) { return point.response < response_floor; }),
      keypoints.end());
  std::sort(keypoints.begin(), keypoints.end(), [](const auto& a, const auto& b) {
    return std::make_tuple(-a.response, a.pt.y, a.pt.x, a.size, a.angle, a.octave, a.class_id) <
        std::make_tuple(-b.response, b.pt.y, b.pt.x, b.size, b.angle, b.octave, b.class_id);
  });
  if (keypoints.size() <= maximum)
    return keypoints;

  const auto suppress = [&](double radius) {
    std::vector<cv::KeyPoint> selected;
    selected.reserve(maximum);
    std::unordered_map<uint64_t, std::vector<cv::Point2f>> cells;
    const auto key = [](int x, int y) {
      return (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 32) | static_cast<uint32_t>(x);
    };
    for (const auto& point : keypoints) {
      const int x = static_cast<int>(point.pt.x / radius);
      const int y = static_cast<int>(point.pt.y / radius);
      bool suppressed = false;
      for (int dy = -1; dy <= 1 && !suppressed; ++dy) {
        for (int dx = -1; dx <= 1 && !suppressed; ++dx) {
          const auto found = cells.find(key(x + dx, y + dy));
          if (found == cells.end())
            continue;
          for (const auto& neighbor : found->second) {
            const cv::Point2f delta = point.pt - neighbor;
            if (delta.dot(delta) < radius * radius) {
              suppressed = true;
              break;
            }
          }
        }
      }
      if (!suppressed) {
        selected.push_back(point);
        cells[key(x, y)].push_back(point.pt);
        if (selected.size() == maximum)
          break;
      }
    }
    return selected;
  };
  std::vector<cv::KeyPoint> selected(keypoints.begin(), keypoints.begin() + maximum);
  double lower = 0;
  double upper = std::hypot(image_size.width, image_size.height);
  // Fixed numerical search precision, not a quality threshold. Remember an
  // actual feasible result; greedy suppression need not be monotonic in radius.
  for (int iteration = 0; iteration < 16; ++iteration) {
    const double radius = (lower + upper) / 2;
    auto candidate = suppress(radius);
    if (candidate.size() == maximum) {
      selected = std::move(candidate);
      lower = radius;
    } else {
      upper = radius;
    }
  }
  return selected;
}

absl::StatusOr<std::vector<cv::DMatch>> MatchQualityBinaryDescriptors(
    const cv::Mat& left,
    const cv::Mat& right,
    float lowe_ratio,
    float maximum_hamming_fraction) {
  if (!std::isfinite(lowe_ratio) || lowe_ratio <= 0 || lowe_ratio >= 1 || !std::isfinite(maximum_hamming_fraction) ||
      maximum_hamming_fraction <= 0 || maximum_hamming_fraction >= 1)
    return absl::InvalidArgumentError("Invalid binary match quality bounds");
  if (left.empty() || right.empty())
    return std::vector<cv::DMatch>{};
  if (left.type() != CV_8UC1 || right.type() != CV_8UC1 || left.cols != right.cols)
    return absl::InvalidArgumentError("Binary descriptors must have equal byte widths");
  if (left.rows < 2 || right.rows < 2)
    return std::vector<cv::DMatch>{};
  cv::BFMatcher matcher(cv::NORM_HAMMING, false);
  std::vector<std::vector<cv::DMatch>> forward, backward;
  matcher.knnMatch(left, right, forward, 2);
  matcher.knnMatch(right, left, backward, 2);
  const float maximum_distance = maximum_hamming_fraction * left.cols * 8.0f;
  const auto eligible = [&](const auto& neighbors) {
    return neighbors.size() == 2 && neighbors[0].distance < lowe_ratio * neighbors[1].distance &&
        neighbors[0].distance <= maximum_distance;
  };
  std::vector<cv::DMatch> accepted;
  for (const auto& neighbors : forward) {
    if (!eligible(neighbors))
      continue;
    const auto& best = neighbors[0];
    const auto& reverse = backward[best.trainIdx];
    if (eligible(reverse) && reverse[0].trainIdx == best.queryIdx)
      accepted.push_back(best);
  }
  return accepted;
}

} // namespace hm::stitching
