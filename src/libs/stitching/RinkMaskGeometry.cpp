#include "hstream/src/libs/stitching/RinkMaskGeometry.h"

#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace hm::stitching {
namespace {
constexpr double kRadians = 3.14159265358979323846 / 180;
struct Sample {
  int x;
  cv::Vec3d ray;
};
bool valid(cv::Vec3d ray) {
  return std::isfinite(ray[0]) && std::isfinite(ray[1]) && std::isfinite(ray[2]) && cv::norm(ray) > 0.5;
}
cv::Vec3d normal(const std::vector<Sample>& samples) {
  cv::Matx33d covariance = cv::Matx33d::zeros();
  for (const auto& sample : samples)
    covariance += sample.ray * sample.ray.t();
  cv::Mat values, vectors;
  cv::eigen(cv::Mat(covariance), values, vectors);
  return {vectors.at<double>(2, 0), vectors.at<double>(2, 1), vectors.at<double>(2, 2)};
}
absl::StatusOr<RinkMaskBoard> suggestBoard(
    const cv::Mat& mask,
    const std::function<std::optional<cv::Vec3d>(double, double)>& ray_at,
    bool near) {
  const std::string side = near ? "near" : "far";
  if (mask.type() != CV_8UC1 || mask.cols < 128 || mask.rows < 32 || mask.cols > 2048 || mask.rows > 2048 || !ray_at)
    return absl::InvalidArgumentError("Rink mask assistance needs a bounded grayscale mask");
  cv::Mat binary, components, stats, centroids;
  cv::compare(mask, 127, binary, cv::CMP_GT);
  const int count = cv::connectedComponentsWithStats(binary, components, stats, centroids);
  if (count < 2)
    return absl::NotFoundError("The rink mask has no visible ice boundary");
  int largest = 1;
  for (int i = 2; i < count; ++i)
    if (stats.at<int>(i, cv::CC_STAT_AREA) > stats.at<int>(largest, cv::CC_STAT_AREA))
      largest = i;
  const int left = stats.at<int>(largest, cv::CC_STAT_LEFT), width = stats.at<int>(largest, cv::CC_STAT_WIDTH);
  if (width < mask.cols / 2 || stats.at<int>(largest, cv::CC_STAT_AREA) < mask.total() / 20)
    return absl::NotFoundError("Too little continuous ice is visible to suggest the " + side + " boards");
  // Exclude the rounded ends and padding; inspect only the requested edge.
  const int start = left + width / 5, end = left + 4 * width / 5;
  std::vector<Sample> samples;
  // The frame edge is not a board measurement. A near edge cropped out in
  // the middle can still be estimated from visible sections on either side.
  std::vector<int> clipped;
  for (int x = start; x < end; ++x) {
    int y = near ? mask.rows - 1 : 0;
    while (y >= 0 && y < mask.rows && components.at<int>(y, x) != largest)
      y += near ? -1 : 1;
    if (y <= std::max(3, mask.rows / 50) || y >= mask.rows - std::max(3, mask.rows / 50)) {
      if (near && y >= mask.rows - std::max(3, mask.rows / 50))
        clipped.push_back(x);
      continue;
    }
    const auto ray = ray_at((x + 0.5) / mask.cols, (y + 0.5) / mask.rows);
    if (ray && valid(*ray))
      samples.push_back({x, *ray / cv::norm(*ray)});
  }
  const auto available = size_t(end - start) - clipped.size();
  if (samples.size() < 64 || samples.size() < size_t(available * (near ? 0.55 : 0.8)))
    return absl::NotFoundError("The " + side + " boundary is clipped or has too many gaps");
  const double tolerance = std::sin((near ? 0.6 : 0.35) * kRadians);
  std::vector<Sample> best;
  // Deterministic broad-baseline hypotheses; no dependence on global RNG state.
  const size_t stride = std::max(size_t(1), samples.size() / 20);
  for (size_t i = 0; i < samples.size() / 2; i += stride) {
    for (size_t j = i + samples.size() / 2; j < samples.size(); j += stride) {
      auto n = samples[i].ray.cross(samples[j].ray);
      if (cv::norm(n) < std::sin(12 * kRadians))
        continue;
      n /= cv::norm(n);
      std::vector<Sample> inliers;
      for (const auto& sample : samples)
        if (std::abs(n.dot(sample.ray)) <= tolerance)
          inliers.push_back(sample);
      if (inliers.size() > best.size())
        best = std::move(inliers);
    }
  }
  // The central search band can still include the starts of rounded corners.
  // Near-board proposals may bridge interruptions, but need broad support.
  const double support = near ? 0.55 : 0.65;
  if (best.size() < samples.size() * support)
    return absl::NotFoundError("No broad straight " + side + "-board section agrees with the camera calibration");
  const auto n = normal(best);
  best.clear();
  for (const auto& sample : samples)
    if (std::abs(n.dot(sample.ray)) <= tolerance)
      best.push_back(sample);
  const int span = best.empty() ? 0 : best.back().x - best.front().x;
  const auto clipped_in_span = std::count_if(clipped.begin(), clipped.end(), [&](int x) {
    return !best.empty() && x >= best.front().x && x <= best.back().x;
  });
  if (best.size() < samples.size() * support || span < width * (near ? 0.2 : 0.4) ||
      best.size() < size_t(span - clipped_in_span) * (near ? 0.55 : 0.9))
    return absl::NotFoundError("The " + side + "-board support is too short or irregular");
  if (near) {
    // Require support across the visible boundary, but do not penalize a
    // middle section lost to the bottom crop. Missing projection coverage or
    // contradictory contours still count against the fit.
    std::array<size_t, 3> thirds{};
    std::array<size_t, 3> visible_thirds{};
    for (int x = best.front().x; x <= best.back().x; ++x)
      if (!std::binary_search(clipped.begin(), clipped.end(), x))
        ++visible_thirds[std::min(2, 3 * (x - best.front().x) / (span + 1))];
    for (const auto& sample : best)
      ++thirds[std::min(2, 3 * (sample.x - best.front().x) / (span + 1))];
    for (size_t i = 0; i < thirds.size(); ++i)
      if (thirds[i] < visible_thirds[i] * 0.24)
        return absl::NotFoundError("Near-board support is missing across too much of the arc");
  } else {
    for (size_t i = 1; i < best.size(); ++i)
      if (best[i].x - best[i - 1].x > std::max(4, width / 40))
        return absl::NotFoundError("Obstructions break the far-board boundary; mark a visible section manually");
  }
  if (std::acos(std::clamp(best.front().ray.dot(best.back().ray), -1.0, 1.0)) < 20 * kRadians)
    return absl::NotFoundError("The visible " + side + " boards are too short for a reliable suggestion");
  RinkMaskBoard result;
  double squared = 0;
  for (const auto& sample : best)
    squared += std::pow(std::asin(std::clamp(n.dot(sample.ray), -1.0, 1.0)), 2);
  result.rms_degrees = std::sqrt(squared / best.size()) / kRadians;
  if (near) {
    // Great-circle interpolation gives a single smooth projected board arc,
    // independent of individual silhouette steps, posts and door openings.
    const auto fitted_normal = normal(best);
    auto first = best.front().ray - fitted_normal * fitted_normal.dot(best.front().ray);
    auto last = best.back().ray - fitted_normal * fitted_normal.dot(best.back().ray);
    first /= cv::norm(first);
    last /= cv::norm(last);
    const double angle = std::acos(std::clamp(first.dot(last), -1.0, 1.0));
    if (angle >= 170 * kRadians)
      return absl::NotFoundError("The near-board arc spans too much of the camera view");
    for (size_t i = 0; i <= 32; ++i) {
      const double t = i / 32.0;
      auto ray = first * std::sin((1 - t) * angle) + last * std::sin(t * angle);
      result.rays.push_back(ray / cv::norm(ray));
    }
  } else {
    for (size_t i = 0; i < 32; ++i)
      result.rays.push_back(best[i * (best.size() - 1) / 31].ray);
  }
  return result;
}
} // namespace

absl::StatusOr<RinkMaskBoard> SuggestFarRinkBoard(
    const cv::Mat& mask,
    const std::function<std::optional<cv::Vec3d>(double, double)>& ray_at) {
  return suggestBoard(mask, ray_at, false);
}

absl::StatusOr<RinkMaskBoard> SuggestNearRinkBoard(
    const cv::Mat& mask,
    const std::function<std::optional<cv::Vec3d>(double, double)>& ray_at) {
  return suggestBoard(mask, ray_at, true);
}
} // namespace hm::stitching
