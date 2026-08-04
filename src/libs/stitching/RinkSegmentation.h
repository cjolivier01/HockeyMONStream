#pragma once

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "absl/status/statusor.h"
#include "hstream/src/libs/onnx/OnnxSession.h"

namespace hm::stitching {

struct RinkInput {
  std::vector<float> tensor;
  int source_width{0};
  int source_height{0};
  int resized_width{0};
  int resized_height{0};
};

struct RinkProfile {
  std::vector<cv::Mat> masks;
  cv::Mat combined_mask;
  cv::Point2d centroid;
  cv::Rect2d combined_bbox;
  std::vector<float> scores;
};

class RinkSegmentation {
 public:
  static constexpr int kInputWidth = 1344;
  static constexpr int kInputHeight = 800;
  static constexpr int kQueryCount = 100;
  static constexpr int kClassCountWithBackground = 81;
  static constexpr int kMaskWidth = 336;
  static constexpr int kMaskHeight = 200;
  static constexpr double kHockeyMomInferenceScale = 0.5;

  static absl::StatusOr<std::unique_ptr<RinkSegmentation>> Create(const std::string& model_path);
  static absl::StatusOr<RinkInput> Prepare(const cv::Mat& bgr_image);
  static absl::StatusOr<RinkProfile> Postprocess(
      const RinkInput& input,
      const float* class_logits,
      size_t class_logit_count,
      const float* mask_logits,
      size_t mask_logit_count);

  absl::StatusOr<RinkProfile> Infer(const cv::Mat& bgr_image, double inference_scale = 1.0) const;

 private:
  explicit RinkSegmentation(std::unique_ptr<hm::onnx::Session> session);
  std::unique_ptr<hm::onnx::Session> session_;
};

} // namespace hm::stitching
