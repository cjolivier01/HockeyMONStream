#include "hstream/src/libs/gpu/Image.h"

#include <cmath>
#include <limits>

namespace hm::gpu {
namespace {
bool Plane(const void* data, size_t pitch, size_t row, size_t height, size_t alignment) {
  if (!data || pitch < row || pitch % alignment || reinterpret_cast<uintptr_t>(data) % alignment ||
      pitch > std::numeric_limits<size_t>::max() / height)
    return false;
  return reinterpret_cast<uintptr_t>(data) <= std::numeric_limits<uintptr_t>::max() - pitch * height;
}
} // namespace
bool ValidateImage(const Image& image) {
  if (image.width <= 0 || image.height <= 0 || image.width >= (1 << 24) || image.height >= (1 << 24))
    return false;
  size_t bytes, alignment;
  switch (image.format) {
    case PixelFormat::kRgba8:
    case PixelFormat::kRgb10A2:
      bytes = alignment = 4;
      break;
    case PixelFormat::kRgb8:
      bytes = 3;
      alignment = 1;
      break;
    case PixelFormat::kGray8:
    case PixelFormat::kNv12:
      bytes = alignment = 1;
      break;
    case PixelFormat::kP010:
      bytes = alignment = 2;
      break;
    default:
      return false;
  }
  if (!Plane(image.plane0, image.pitch0, image.width * bytes, image.height, alignment))
    return false;
  if (image.format != PixelFormat::kNv12 && image.format != PixelFormat::kP010)
    return true;
  return image.width % 2 == 0 && image.height % 2 == 0 &&
      Plane(image.plane1, image.pitch1, image.width * bytes, image.height / 2, alignment) &&
      std::isfinite(image.y_offset) && std::isfinite(image.uv_offset) && std::isfinite(image.y_scale) &&
      std::isfinite(image.r_v) && std::isfinite(image.g_u) && std::isfinite(image.g_v) && std::isfinite(image.b_u);
}
} // namespace hm::gpu
