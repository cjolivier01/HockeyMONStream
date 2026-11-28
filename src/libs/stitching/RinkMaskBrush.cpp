#include "hstream/src/libs/stitching/RinkMaskBrush.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace hm::stitching {
namespace {
int center_sample(int pixel, int native_extent, int preview_extent) {
  return static_cast<int>((2LL * pixel + 1) * native_extent / (2LL * preview_extent));
}
} // namespace

cv::Mat SampleIceBrushPreview(const cv::Mat& mask, cv::Size preview) {
  if (mask.size() == preview)
    return mask;
  std::vector<int> columns(preview.width);
  for (int x = 0; x < preview.width; ++x)
    columns[x] = center_sample(x, mask.cols, preview.width);
  cv::Mat sampled(preview, CV_8U);
  for (int y = 0; y < preview.height; ++y) {
    const auto* source = mask.ptr<uint8_t>(center_sample(y, mask.rows, preview.height));
    auto* row = sampled.ptr<uint8_t>(y);
    for (int x = 0; x < preview.width; ++x)
      row[x] = source[columns[x]];
  }
  return sampled;
}

// Fill the round-capped segment covered by the preview brush. OpenCV's thick line
// rounds odd diameters up and draws both edge pixels, making an eraser wider than its cursor.
void DrawIceBrushSegment(
    cv::Mat& mask,
    cv::Mat* erased,
    cv::Size preview,
    cv::Point start,
    cv::Point end,
    int diameter,
    uint8_t value) {
  const cv::Point2d a(start.x + .5, start.y + .5), b(end.x + .5, end.y + .5), delta = b - a;
  const double radius = diameter / 2.;
  const double length = cv::norm(delta);
  const cv::Point2d normal = length ? cv::Point2d(-delta.y, delta.x) * (radius / length) : cv::Point2d();
  const double scale_x = static_cast<double>(mask.cols) / preview.width;
  const double scale_y = static_cast<double>(mask.rows) / preview.height;
  // Native samples lie at half-integers. Half-open spans give an even-width brush a consistent edge.
  auto first_pixel = [](double edge, double scale, int limit) {
    return std::clamp(static_cast<int>(std::ceil(edge * scale - .5)), 0, limit);
  };
  const int first_y = first_pixel(std::min(a.y, b.y) - radius, scale_y, mask.rows);
  const int end_y = first_pixel(std::max(a.y, b.y) + radius, scale_y, mask.rows);
  for (int row = first_y; row < end_y; ++row) {
    const double y = (row + .5) / scale_y;
    double left = std::numeric_limits<double>::infinity(), right = -left;
    // A horizontal slice of a capsule is one span bounded by its round caps or straight sides.
    for (const auto& center : {a, b}) {
      const double dy = y - center.y;
      if (std::abs(dy) <= radius) {
        const double dx = std::sqrt(radius * radius - dy * dy);
        left = std::min(left, center.x - dx);
        right = std::max(right, center.x + dx);
      }
    }
    if (delta.y != 0) {
      for (const auto& side : {a + normal, a - normal}) {
        const double t = (y - side.y) / delta.y;
        if (t >= 0 && t <= 1) {
          const double x = side.x + t * delta.x;
          left = std::min(left, x);
          right = std::max(right, x);
        }
      }
    }
    if (left >= right)
      continue;
    const int begin = first_pixel(left, scale_x, mask.cols);
    const int past = first_pixel(right, scale_x, mask.cols);
    auto* pixels = mask.ptr<uint8_t>(row);
    std::fill(pixels + begin, pixels + past, value);
    if (erased) {
      auto* pixels_erased = erased->ptr<uint8_t>(row);
      std::fill(pixels_erased + begin, pixels_erased + past, 255);
    }
  }
  if (diameter == 1 && mask.size() != preview) {
    // At fractional scale a one-pixel stroke can pass between native pixel centers.
    // Keep the sampled preview centerline continuous, including single clicks.
    cv::LineIterator line(preview, start, end, 8, true);
    for (int i = 0; i < line.count; ++i, ++line) {
      const auto point = line.pos();
      const cv::Point sample(
          center_sample(point.x, mask.cols, preview.width), center_sample(point.y, mask.rows, preview.height));
      mask.at<uint8_t>(sample) = value;
      if (erased)
        erased->at<uint8_t>(sample) = 255;
    }
  }
}

} // namespace hm::stitching
