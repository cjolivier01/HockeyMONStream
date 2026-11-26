#include "hstream/src/libs/scoreboard/CameraRegion.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace hm::scoreboard {
absl::StatusOr<CameraRegion> MakeCameraRegion(
    const std::array<std::array<float, 2>, 4>& polygon,
    int source_width,
    int source_height,
    int alignment) {
  if (source_width < 2 || source_height < 2 || alignment < 1 || source_width % alignment || source_height % alignment)
    return absl::InvalidArgumentError("Scoreboard camera geometry cannot satisfy crop alignment");
  double min_x = source_width, min_y = source_height, max_x = 0, max_y = 0;
  std::vector<cv::Point2f> points;
  for (const auto& point : polygon) {
    const double x = point[0], y = point[1];
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x > source_width - 1 || y > source_height - 1)
      return absl::InvalidArgumentError("Scoreboard camera polygon lies outside the source");
    min_x = std::min(min_x, x);
    min_y = std::min(min_y, y);
    max_x = std::max(max_x, x);
    max_y = std::max(max_y, y);
    points.emplace_back(point[0], point[1]);
  }
  if (!cv::isContourConvex(points) || cv::contourArea(points) < 1)
    return absl::InvalidArgumentError("Scoreboard camera polygon must be a nondegenerate convex quadrilateral");
  const int left = static_cast<int>(std::floor(min_x)) / alignment * alignment;
  const int top = static_cast<int>(std::floor(min_y)) / alignment * alignment;
  const auto end = [alignment](double maximum, int extent) {
    const int64_t exclusive = static_cast<int64_t>(std::ceil(maximum)) + 1;
    return static_cast<int>(std::min<int64_t>((exclusive + alignment - 1) / alignment * alignment, extent));
  };
  CameraRegion result{left, top, end(max_x, source_width) - left, end(max_y, source_height) - top, polygon};
  for (auto& point : result.polygon) {
    point[0] -= left;
    point[1] -= top;
  }
  return result;
}

absl::StatusOr<gpu::Image> CropCameraImage(const gpu::Image& source, const CameraRegion& region) {
  if (!gpu::ValidateImage(source) || region.left < 0 || region.top < 0 || region.width < 2 || region.height < 2 ||
      region.width > source.width || region.height > source.height || region.left > source.width - region.width ||
      region.top > source.height - region.height)
    return absl::InvalidArgumentError("Scoreboard GPU crop lies outside its source image");
  const bool yuv = source.format == gpu::PixelFormat::kNv12 || source.format == gpu::PixelFormat::kP010;
  if (yuv && (region.left % 2 || region.top % 2 || region.width % 2 || region.height % 2))
    return absl::InvalidArgumentError("Scoreboard YUV crop must preserve chroma alignment");
  size_t bytes = 0;
  switch (source.format) {
    case gpu::PixelFormat::kGray8:
    case gpu::PixelFormat::kNv12:
      bytes = 1;
      break;
    case gpu::PixelFormat::kP010:
      bytes = 2;
      break;
    case gpu::PixelFormat::kRgb8:
      bytes = 3;
      break;
    case gpu::PixelFormat::kRgba8:
    case gpu::PixelFormat::kRgb10A2:
      bytes = 4;
      break;
    case gpu::PixelFormat::kRgba16Float:
      bytes = 8;
      break;
  }
  gpu::Image result = source;
  result.width = region.width;
  result.height = region.height;
  result.plane0 =
      static_cast<const uint8_t*>(source.plane0) + size_t(region.top) * source.pitch0 + size_t(region.left) * bytes;
  if (yuv)
    result.plane1 = static_cast<const uint8_t*>(source.plane1) + size_t(region.top / 2) * source.pitch1 +
        size_t(region.left) * bytes;
  if (!gpu::ValidateImage(result))
    return absl::InvalidArgumentError("Scoreboard GPU crop has invalid plane geometry");
  return result;
}
} // namespace hm::scoreboard
