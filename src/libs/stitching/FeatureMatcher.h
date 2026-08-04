#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "absl/status/statusor.h"
#include "hstream/src/libs/onnx/OnnxSession.h"

namespace hm::stitching {

struct FeaturePairInput {
  std::vector<float> tensor;
  cv::Size source_sizes[2];
  cv::Size resized_sizes[2];
};

struct FeatureMatch {
  cv::Point2f left;
  cv::Point2f right;
  float score{0.0f};
};

struct FeatureMatchResult {
  size_t accepted_match_count{0};
  std::vector<FeatureMatch> accepted;
  std::vector<FeatureMatch> selected;
};

class FeatureMatcher {
 public:
  static constexpr int kInputWidth = 1024;
  static constexpr int kInputHeight = 576;
  static constexpr int kKeypointsPerImage = 2048;
  static constexpr float kMinimumScore = 0.2f;

  static absl::StatusOr<std::unique_ptr<FeatureMatcher>> Create(const std::string& model_path);
  static absl::StatusOr<FeaturePairInput> Prepare(const cv::Mat& left_bgr, const cv::Mat& right_bgr);
  static absl::StatusOr<FeatureMatchResult> Postprocess(
      const FeaturePairInput& input,
      const float* keypoints,
      size_t keypoint_count,
      const int64_t* matches,
      size_t match_value_count,
      const float* scores,
      size_t score_count,
      size_t max_control_points);

  absl::StatusOr<FeatureMatchResult> Infer(const cv::Mat& left_bgr, const cv::Mat& right_bgr, size_t max_control_points)
      const;

 private:
  explicit FeatureMatcher(std::unique_ptr<hm::onnx::Session> session);
  std::unique_ptr<hm::onnx::Session> session_;
};

} // namespace hm::stitching
