#pragma once

#include <opencv2/core.hpp>

namespace hm::detection {
// The saved calibration mask is CPU metadata. These helpers never read video.
cv::Mat convert_to_bit_mask(const cv::Mat& mask);
cv::Point2f compute_centroid(const cv::Mat& mask, cv::Rect2i& bounds);

struct RinkDetectionDecision {
  bool keep{false};
  cv::Point2f center, bottom; // Unscaled diagnostic points, before the side shift.
  int raised_center{0}, lowered_bottom{0};
};
// Same upper-half feet / lower-half center rule used by ds-fieldmask. The
// caller binds the mask and scales to the current published frame geometry.
RinkDetectionDecision EvaluateRinkDetection(
    const cv::Mat& packed_mask,
    cv::Size mask_size,
    cv::Point2f centroid,
    cv::Rect2f box,
    cv::Point2f scale,
    float raise_center_ratio,
    float lower_bottom_ratio);
} // namespace hm::detection
