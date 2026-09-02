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
#include "hstream/src/libs/stitching/ControlPointMatcher.h"

namespace hm::stitching {

struct FeaturePairInput {
  std::vector<float> tensor;
  cv::Size source_sizes[2];
  cv::Size resized_sizes[2];
  cv::Size tensor_size;
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
  static constexpr int kKeypointsPerImage = 1024;
  static constexpr int kLegacyAlikedKeypointsPerImage = 2048;
  static constexpr int kLoFTRMaximumDimension = 1600;
  static constexpr int kLoFTRDimensionAlignment = 32;
  static constexpr int kAkazeMaximumDimension = 1920;
  static constexpr int kAkazeMaximumKeypoints = 2000;
  static constexpr float kMinimumScore = 0.2f;
  static constexpr float kAkazeThreshold = 0.0001f;
  static constexpr float kAkazeLoweRatio = 0.75f;

  static absl::StatusOr<std::unique_ptr<FeatureMatcher>> Create(
      const std::string& model_path,
      ControlPointMatcher matcher = ControlPointMatcher::kSuperPointLightGlue);
  // The release qualification oracle predates the selectable production
  // backends and uses a frozen RGB RaCo-ALIKED k2048 graph. Keep its contract
  // explicit so it cannot be mistaken for the production SuperPoint graph.
  static absl::StatusOr<std::unique_ptr<FeatureMatcher>> CreateLegacyAlikedParity(const std::string& model_path);
  static absl::StatusOr<FeaturePairInput> Prepare(const cv::Mat& left_bgr, const cv::Mat& right_bgr);
  static absl::StatusOr<FeaturePairInput> PrepareLoFTR(const cv::Mat& left_bgr, const cv::Mat& right_bgr);
  static absl::StatusOr<FeatureMatchResult> Postprocess(
      const FeaturePairInput& input,
      const float* keypoints,
      size_t keypoint_count,
      const int64_t* matches,
      size_t match_value_count,
      const float* scores,
      size_t score_count,
      size_t max_control_points);
  static absl::StatusOr<FeatureMatchResult> PostprocessDeDoDe(
      const FeaturePairInput& input,
      const float* keypoints,
      size_t keypoint_count,
      const int64_t* matches,
      size_t match_count,
      const float* scores,
      size_t score_count,
      size_t max_control_points);
  static absl::StatusOr<FeatureMatchResult> PostprocessLoFTR(
      const FeaturePairInput& input,
      const float* left_keypoints,
      size_t left_keypoint_count,
      const float* right_keypoints,
      size_t right_keypoint_count,
      const float* scores,
      size_t score_count,
      size_t max_control_points);
  static absl::StatusOr<std::vector<FeatureMatch>> SelectControlPoints(
      const std::vector<FeatureMatch>& accepted,
      cv::Size left_source_size,
      size_t max_control_points);

  absl::StatusOr<FeatureMatchResult> Infer(
      const cv::Mat& left_bgr,
      const cv::Mat& right_bgr,
      size_t max_control_points,
      const std::function<void()>& inference_complete = {},
      const std::function<bool()>& is_cancelled = {}) const;

 private:
  FeatureMatcher(
      ControlPointMatcher matcher,
      std::unique_ptr<hm::onnx::Session> session = {},
      int input_channels = 0,
      size_t sparse_keypoints_per_image = kKeypointsPerImage,
      bool sparse_keypoints_are_float = false);
  static absl::StatusOr<FeatureMatchResult> PostprocessSparse(
      const FeaturePairInput& input,
      const float* keypoints,
      size_t keypoint_count,
      const int64_t* matches,
      size_t match_value_count,
      const float* scores,
      size_t score_count,
      size_t max_control_points,
      size_t keypoints_per_image);
  absl::StatusOr<FeatureMatchResult> InferAkaze(
      const cv::Mat& left_bgr,
      const cv::Mat& right_bgr,
      size_t max_control_points,
      const std::function<void()>& inference_complete,
      const std::function<bool()>& is_cancelled) const;

  ControlPointMatcher matcher_{ControlPointMatcher::kSuperPointLightGlue};
  std::unique_ptr<hm::onnx::Session> session_;
  int input_channels_{0};
  size_t sparse_keypoints_per_image_{kKeypointsPerImage};
  bool sparse_keypoints_are_float_{false};
};

} // namespace hm::stitching
