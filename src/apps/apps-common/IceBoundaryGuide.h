#pragma once

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <vector>

namespace hm::gpu_preview {

// Preview-only bounded copies of the already CPU-resident calibration mask.
inline cv::Mat bounded_rink_mask(const cv::Mat& mask) {
  const float scale = std::min(1.0F, 2048.0F / std::max(mask.cols, mask.rows));
  cv::Mat result;
  cv::resize(
      mask,
      result,
      {std::max(1, cvRound(mask.cols * scale)), std::max(1, cvRound(mask.rows * scale))},
      0,
      0,
      cv::INTER_NEAREST);
  return result;
}

inline std::vector<std::vector<cv::Point2f>> rink_mask_contours(
    const cv::Mat& bounded,
    int canvas_width,
    int canvas_height) {
  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(bounded, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
  std::vector<std::vector<cv::Point2f>> result;
  for (const auto& contour : contours) {
    auto& path = result.emplace_back();
    for (const auto& point : contour)
      path.emplace_back(point.x * float(canvas_width) / bounded.cols, point.y * float(canvas_height) / bounded.rows);
  }
  return result;
}

} // namespace hm::gpu_preview
