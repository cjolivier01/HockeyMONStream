#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "hstream/src/libs/onnx/OnnxSession.h"

namespace hm::stitching {

struct RinkLandmarkDetection {
  int class_index{0};
  std::string label;
  double score{0};
  std::vector<cv::Point2d> outline;
  // Samples along painted line centres. Empty for areas/objects: their image
  // centroid is not a physical rink landmark, especially for a raised net.
  std::vector<cv::Point2d> line_samples;
};

class RinkLandmarks {
 public:
  static constexpr int kCanvas = 1024;
  static constexpr int kResize = 1008;
  static constexpr int kQueries = 100;
  static constexpr int kClasses = 12;
  static constexpr int kMaskSize = 256;
  static absl::StatusOr<std::unique_ptr<RinkLandmarks>> Create(
      const std::string& model_path,
      hm::onnx::ExecutionProvider provider = hm::onnx::ExecutionProvider::kCuda);
  // Offline, bounded calibration stills only. No steady-state video readback.
  absl::StatusOr<std::vector<RinkLandmarkDetection>> Infer(
      const cv::Mat& bgr,
      const std::function<bool()>& cancelled = {});
  static absl::StatusOr<std::vector<RinkLandmarkDetection>> Postprocess(
      const float* classes,
      size_t class_count,
      const float* masks,
      size_t mask_count,
      cv::Size resized,
      cv::Size original);

 private:
  explicit RinkLandmarks(std::unique_ptr<hm::onnx::Session> session) : session_(std::move(session)) {}
  std::unique_ptr<hm::onnx::Session> session_;
};

} // namespace hm::stitching
