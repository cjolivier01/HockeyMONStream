#include "hstream/src/libs/scoreboard/ScoreboardPlan.h"

#include <opencv2/imgproc.hpp>

#include <cmath>
#include <limits>

namespace hm::scoreboard {
bool ValidatePlan(const Plan& p) {
  if (p.source_width < 2 || p.source_height < 2 || p.output_width < 2 || p.output_height < 2 || p.width < 2 ||
      p.height < 2 || p.width > p.output_width || p.height > p.output_height || p.left < 0 || p.top < 0 ||
      p.left > p.output_width - p.width || p.top > p.output_height - p.height)
    return false;
  for (const auto number : p.inverse)
    if (!std::isfinite(number))
      return false;
  const auto& h = p.inverse;
  const double determinant = h[0] * (double(h[4]) * h[8] - double(h[5]) * h[7]) -
      h[1] * (double(h[3]) * h[8] - double(h[5]) * h[6]) + h[2] * (double(h[3]) * h[7] - double(h[4]) * h[6]);
  if (!std::isfinite(determinant) || determinant == 0)
    return false;
  // A projective pole cannot cross a rectangle if all four corner
  // denominators have the same sign. Check mapped extents before GPU reads.
  int sign = 0;
  for (const auto x : {0, p.width - 1}) {
    for (const auto y : {0, p.height - 1}) {
      const double d = double(h[6]) * x + double(h[7]) * y + h[8];
      if (!std::isfinite(d) || std::abs(d) < 1e-7 || (sign && sign != (d > 0 ? 1 : -1)))
        return false;
      sign = d > 0 ? 1 : -1;
      const double sx = (double(h[0]) * x + double(h[1]) * y + h[2]) / d;
      const double sy = (double(h[3]) * x + double(h[4]) * y + h[5]) / d;
      if (!std::isfinite(sx) || !std::isfinite(sy) || sx < -0.01 || sy < -0.01 || sx > p.source_width - 1 + 0.01 ||
          sy > p.source_height - 1 + 0.01)
        return false;
    }
  }
  return true;
}
absl::StatusOr<Plan> MakePlan(
    const std::array<std::array<float, 2>, 4>& polygon,
    int source_width,
    int source_height,
    int output_width,
    int output_height,
    int requested_width,
    int requested_height,
    Position position) {
  if (source_width < 2 || source_height < 2 || output_width < 2 || output_height < 2 || requested_width < 2 ||
      requested_height < 2 || requested_width > output_width || requested_height > output_height ||
      (position != Position::kUpperLeft && position != Position::kUpperCenter && position != Position::kUpperRight))
    return absl::InvalidArgumentError("Scoreboard requires explicit positive dimensions within the output");
  std::vector<cv::Point2f> source;
  for (const auto& point : polygon) {
    if (!std::isfinite(point[0]) || !std::isfinite(point[1]) || point[0] < 0 || point[1] < 0 ||
        double(point[0]) > source_width - 1 || double(point[1]) > source_height - 1)
      return absl::InvalidArgumentError("Scoreboard polygon lies outside the current panorama");
    source.emplace_back(point[0], point[1]);
  }
  if (!cv::isContourConvex(source) || cv::contourArea(source) < 1)
    return absl::InvalidArgumentError("Scoreboard polygon must be a nondegenerate convex quadrilateral");
  const double aspect = (cv::norm(source[1] - source[0]) + cv::norm(source[2] - source[3])) /
      (cv::norm(source[3] - source[0]) + cv::norm(source[2] - source[1]));
  const double width_candidate = std::trunc(requested_height * aspect);
  const double height_candidate = std::trunc(requested_width / aspect);
  if (!std::isfinite(width_candidate) || !std::isfinite(height_candidate))
    return absl::InvalidArgumentError("Scoreboard aspect is not finite");
  int width = requested_width, height = requested_height;
  if (std::abs(requested_width - width_candidate) / requested_width <
      std::abs(requested_height - height_candidate) / requested_height) {
    if (width_candidate < 2 || width_candidate > output_width)
      return absl::InvalidArgumentError("Scoreboard aspect-adjusted width exceeds output");
    width = static_cast<int>(width_candidate);
  } else {
    if (height_candidate < 2 || height_candidate > output_height)
      return absl::InvalidArgumentError("Scoreboard aspect-adjusted height exceeds output");
    height = static_cast<int>(height_candidate);
  }
  const std::vector<cv::Point2f> destination{
      {0, 0}, {float(width - 1), 0}, {float(width - 1), float(height - 1)}, {0, float(height - 1)}};
  const auto inverse = cv::getPerspectiveTransform(destination, source);
  Plan plan{
      source_width,
      source_height,
      output_width,
      output_height,
      HorizontalOffset(position, output_width, width),
      0,
      width,
      height,
      {}};
  for (int i = 0; i < 9; ++i)
    plan.inverse[i] = inverse.at<double>(i / 3, i % 3);
  if (!ValidatePlan(plan))
    return absl::InvalidArgumentError("Scoreboard transform is singular or outside the panorama");
  return plan;
}
} // namespace hm::scoreboard
