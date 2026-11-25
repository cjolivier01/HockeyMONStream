#pragma once

#include <opencv2/core/types.hpp>
#include <array>

#include "hstream/src/libs/scoreboard/ScoreboardPlacement.h"

namespace hm::scoreboard {
// Compose a reflection in the final rectangle with the perspective warp.
inline std::array<cv::Point2f, 4> RectangleCorners(int width, int height, Orientation flip) {
  const float x = width - 1, y = height - 1;
  const float left = flip.horizontal ? x : 0, right = x - left;
  const float top = flip.vertical ? y : 0, bottom = y - top;
  return {cv::Point2f(left, top), {right, top}, {right, bottom}, {left, bottom}};
}
} // namespace hm::scoreboard
