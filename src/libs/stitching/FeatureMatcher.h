#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "absl/status/statusor.h"
#include "hstream/src/libs/onnx/OnnxSession.h"

namespace hm::stitching {

enum class ControlPointMatcher {
  kAlikedLightGlue,
};

const char* ControlPointMatcherName(ControlPointMatcher matcher);
absl::StatusOr<ControlPointMatcher> ParseControlPointMatcher(const std::string& value);

struct FeaturePairInput {
  std::vector<float> tensor;
  cv::Size source_sizes[2];
  cv::Size resized_sizes[2];
};

struct FeatureMatch {
  cv::Point2f left;
  cv::Point2f right;
  float score{0.0f};
  int left_index{-1};
  int right_index{-1};
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

  static absl::StatusOr<std::unique_ptr<FeatureMatcher>> Create(
      const std::string& model_path,
      ControlPointMatcher matcher = ControlPointMatcher::kAlikedLightGlue);
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

  absl::StatusOr<FeatureMatchResult> Infer(
      const cv::Mat& left_bgr,
      const cv::Mat& right_bgr,
      size_t max_control_points,
      const std::function<void()>& inference_complete = {},
      const std::function<bool()>& is_cancelled = {}) const;

 private:
  explicit FeatureMatcher(std::unique_ptr<hm::onnx::Session> session);
  std::unique_ptr<hm::onnx::Session> session_;
};

} // namespace hm::stitching
