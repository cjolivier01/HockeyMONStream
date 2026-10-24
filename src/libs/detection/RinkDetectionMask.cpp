#include "hstream/src/libs/detection/RinkDetectionMask.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace hm::detection {

namespace {
bool is_bit_set(const cv::Mat& mask, const cv::Point& point) {
  const auto byte = mask.ptr<uint8_t>(point.y)[point.x / 8];
  return (byte & (1 << (7 - point.x % 8))) != 0;
}
} // namespace

// Convert 8-bit-per-pixel mask to 1-bit-per-pixel (packed as bytes)
cv::Mat convert_to_bit_mask(const cv::Mat& inputMask) {
  if (inputMask.type() != CV_8UC1) {
    throw std::invalid_argument("Input mask must be a CV_8UC1 matrix.");
  }

  // Output matrix: 1/8th the number of columns (rounded up), same rows
  int packedCols = inputMask.cols / 8 + (inputMask.cols % 8 != 0);
  cv::Mat bitMask(inputMask.rows, packedCols, CV_8UC1, cv::Scalar(0));

  for (int y = 0; y < inputMask.rows; ++y) {
    const uchar* inputRow = inputMask.ptr<uchar>(y);
    uchar* outputRow = bitMask.ptr<uchar>(y);

    for (int x = 0; x < inputMask.cols; ++x) {
      if (inputRow[x] != 0) {
        outputRow[x / 8] |= (1 << (7 - (x % 8))); // Set the corresponding bit
      }
    }
  }

  return bitMask;
}

// Compute the centroid of a binary mask
cv::Point2f compute_centroid(const cv::Mat& mask, cv::Rect2i& bbox) {
  if (mask.type() != CV_8UC1) {
    throw std::invalid_argument("Input mask must be a CV_8UC1 matrix.");
  }
  double sumX = 0.0, sumY = 0.0;
  int64_t count = 0;

  int low_x = std::numeric_limits<int>::max(), low_y = std::numeric_limits<int>::max();
  int high_x = -1, high_y = -1;

  for (int y = 0; y < mask.rows; ++y) {
    const uchar* row = mask.ptr<uchar>(y);
    for (int x = 0; x < mask.cols; ++x) {
      if (row[x] != 0) { // Check for non-zero values
        sumX += x;
        sumY += y;
        low_x = std::min(x, low_x);
        low_y = std::min(y, low_y);
        high_x = std::max(x, high_x);
        high_y = std::max(y, high_y);
        ++count;
      }
    }
  }

  if (count == 0) {
    throw std::runtime_error("The mask has no non-zero pixels.");
  }

  if (low_x != std::numeric_limits<int>::max() && low_y != std::numeric_limits<int>::max() && high_x > 0 &&
      high_y > 0) {
    bbox = cv::Rect2i(low_x, low_y, high_x - low_x, high_y - low_y);
  } else {
    bbox = cv::Rect2i(0, 0, 0, 0);
  }

  // Return the centroid as a floating-point point
  return cv::Point2f(sumX / count, sumY / count);
}

RinkDetectionDecision EvaluateRinkDetection(
    const cv::Mat& packed_mask,
    cv::Size size,
    cv::Point2f centroid,
    cv::Rect2f box,
    cv::Point2f scale,
    float raise,
    float lower) {
  RinkDetectionDecision result;
  const float raised = box.height * raise, lowered = box.height * lower;
  if (size.width <= 0 || size.height <= 0 || packed_mask.type() != CV_8UC1 || packed_mask.rows != size.height ||
      packed_mask.cols != size.width / 8 + (size.width % 8 != 0) || !std::isfinite(box.x) || !std::isfinite(box.y) ||
      !std::isfinite(box.width) || !std::isfinite(box.height) || box.width <= 0 || box.height <= 0 ||
      !std::isfinite(scale.x) || !std::isfinite(scale.y) || scale.x <= 0 || scale.y <= 0 ||
      !std::isfinite(centroid.x) || !std::isfinite(centroid.y) || !std::isfinite(raise) || !std::isfinite(lower) ||
      std::abs(double(raised)) >= std::numeric_limits<int>::max() ||
      std::abs(double(lowered)) >= std::numeric_limits<int>::max())
    return result;
  result.raised_center = raised;
  result.lowered_bottom = lowered;
  const float half_width = box.width / 2;
  result.center = {box.x + half_width, box.y + box.height / 2 - result.raised_center};
  result.bottom = {box.x + half_width, box.y + box.height - result.lowered_bottom};
  auto bottom = result.bottom;
  // Preserve the existing side adjustment before source/pipeline scaling.
  bottom.x += (bottom.x <= centroid.x ? 1 : -1) * half_width * 0.2f;
  bottom.x *= scale.x;
  bottom.y *= scale.y;
  auto center = result.center;
  center.x *= scale.x;
  center.y *= scale.y;
  if (!std::isfinite(bottom.x) || !std::isfinite(bottom.y) || !std::isfinite(center.x) || !std::isfinite(center.y))
    return result;
  auto point = bottom.y <= centroid.y ? bottom : center;
  point.x = std::clamp(point.x, 0.f, float(size.width - 1));
  point.y = std::clamp(point.y, 0.f, float(size.height - 1));
  result.keep = is_bit_set(packed_mask, point);
  return result;
}
} // namespace hm::detection
