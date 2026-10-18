#include "hstream/src/libs/stitching/RinkMaskGeometry.h"

#include <opencv2/imgproc.hpp>
#include <algorithm>
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
} // namespace

absl::StatusOr<RinkMaskBoard> SuggestFarRinkBoard(
    const cv::Mat& mask,
    const std::function<std::optional<cv::Vec3d>(double, double)>& ray_at) {
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
    return absl::NotFoundError("Too little continuous ice is visible to suggest the far boards");
  // Exclude the rounded ends and padding. Only the TOP silhouette is read.
  const int start = left + width / 5, end = left + 4 * width / 5;
  std::vector<Sample> samples;
  for (int x = start; x < end; ++x) {
    int y = 0;
    while (y < mask.rows && components.at<int>(y, x) != largest)
      ++y;
    if (y <= std::max(3, mask.rows / 50) || y >= mask.rows - std::max(3, mask.rows / 50))
      continue;
    const auto ray = ray_at((x + 0.5) / mask.cols, (y + 0.5) / mask.rows);
    if (ray && valid(*ray))
      samples.push_back({x, *ray / cv::norm(*ray)});
  }
  if (samples.size() < 64 || samples.size() < size_t((end - start) * 0.8))
    return absl::NotFoundError("The far boundary is clipped or has too many gaps");
  const double tolerance = std::sin(0.35 * kRadians);
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
  // Trim its unsupported ends, but do not bridge internal occlusions.
  if (best.size() < samples.size() * 0.65)
    return absl::NotFoundError("No broad straight far-board section agrees with the camera calibration");
  const auto n = normal(best);
  best.clear();
  for (const auto& sample : samples)
    if (std::abs(n.dot(sample.ray)) <= tolerance)
      best.push_back(sample);
  if (best.size() < samples.size() * 0.65 || best.back().x - best.front().x < width * 0.4 ||
      best.size() < size_t(best.back().x - best.front().x) * 0.9)
    return absl::NotFoundError("Far-board support is too short or irregular");
  for (size_t i = 1; i < best.size(); ++i)
    if (best[i].x - best[i - 1].x > std::max(4, width / 40))
      return absl::NotFoundError("Obstructions break the far-board boundary; mark a visible section manually");
  if (std::acos(std::clamp(best.front().ray.dot(best.back().ray), -1.0, 1.0)) < 20 * kRadians)
    return absl::NotFoundError("The visible far boards are too short for a reliable suggestion");
  RinkMaskBoard result;
  double squared = 0;
  for (const auto& sample : best)
    squared += std::pow(std::asin(std::clamp(n.dot(sample.ray), -1.0, 1.0)), 2);
  result.rms_degrees = std::sqrt(squared / best.size()) / kRadians;
  for (size_t i = 0; i < 32; ++i)
    result.rays.push_back(best[i * (best.size() - 1) / 31].ray);
  return result;
}
} // namespace hm::stitching
