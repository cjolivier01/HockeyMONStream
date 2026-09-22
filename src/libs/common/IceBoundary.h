#pragma once

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace hm::fieldmask {

// Fractions of a detection's height (vertical) or half width (horizontal).
// Defaults for the application are resolved from ice_boundaries in baseline.yaml.
struct IceBoundaryOffsets {
  float raise_center{0.0F};
  float lower_bottom{0.0F};
  float left{0.2F};
  float right{0.2F};

  bool operator==(const IceBoundaryOffsets& other) const {
    return raise_center == other.raise_center && lower_bottom == other.lower_bottom && left == other.left &&
        right == other.right;
  }
};

struct IceBoundaryPoint {
  float x;
  float y;
};

struct RinkMaskInsets {
  int top{0};
  int bottom{0};
  int left{0};
  int right{0};
  bool operator==(const RinkMaskInsets& other) const {
    return top == other.top && bottom == other.bottom && left == other.left && right == other.right;
  }
};

// Positive values exclude pixels inward from the named edge; negative values
// extend the mask outward. One-sided morphology preserves the curved mask
// boundary and holes, with a defined top/bottom/left/right application order.
// Zero insets share the immutable original mask without allocating a copy.
inline cv::Mat inset_rink_mask(const cv::Mat& original, const RinkMaskInsets& insets) {
  cv::Mat result = original;
  const int values[] = {insets.top, insets.bottom, insets.left, insets.right};
  for (int side = 0; side < 4; ++side) {
    const int value = values[side];
    if (value < -4096 || value > 4096)
      throw std::invalid_argument("Rink mask insets must be from -4096 to 4096 pixels");
    if (!value)
      continue;
    const int amount = std::abs(value);
    const bool vertical = side < 2;
    const bool leading = side == 0 || side == 2;
    const int anchor = (leading == (value > 0)) ? amount : 0;
    const auto kernel =
        cv::getStructuringElement(cv::MORPH_RECT, vertical ? cv::Size(1, amount + 1) : cv::Size(amount + 1, 1));
    cv::Mat next;
    const cv::Point origin = vertical ? cv::Point(0, anchor) : cv::Point(anchor, 0);
    if (value > 0)
      cv::erode(result, next, kernel, origin, 1, cv::BORDER_CONSTANT, cv::Scalar(0));
    else
      cv::dilate(result, next, kernel, origin, 1, cv::BORDER_CONSTANT, cv::Scalar(0));
    result = std::move(next);
  }
  return result;
}

struct IceBoundarySamples {
  IceBoundaryPoint feet;
  IceBoundaryPoint center;
  bool uses_feet;
};

inline IceBoundarySamples ice_boundary_samples(
    float center_x,
    float bottom_y,
    float width,
    float height,
    IceBoundaryPoint centroid,
    const IceBoundaryOffsets& offsets) {
  const float bottom = bottom_y - std::trunc(height * offsets.lower_bottom);
  return {
      {center_x + width * 0.5F * (center_x <= centroid.x ? offsets.left : -offsets.right), bottom},
      {center_x, bottom_y - height * 0.5F - std::trunc(height * offsets.raise_center)},
      bottom <= centroid.y};
}

// Input box, centroid, and output sample use the same coordinate space. The
// top half tests adjusted feet; the bottom half tests the adjusted box center.
// Truncation of vertical offsets preserves the original pruning algorithm.
inline IceBoundaryPoint ice_boundary_sample(
    float center_x,
    float bottom_y,
    float width,
    float height,
    IceBoundaryPoint centroid,
    const IceBoundaryOffsets& offsets) {
  const auto samples = ice_boundary_samples(center_x, bottom_y, width, height, centroid, offsets);
  return samples.uses_feet ? samples.feet : samples.center;
}

} // namespace hm::fieldmask
