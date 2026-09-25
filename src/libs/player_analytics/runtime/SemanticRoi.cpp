#include "hstream/src/libs/player_analytics/runtime/SemanticRoi.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hm::player_analytics {
namespace {
bool Coordinate(float value) {
  return std::isfinite(value) && std::abs(value) <= 1000000;
}
} // namespace

JerseyCropSelection MakeJerseyCrop(
    const ImageView& image,
    const Box& box,
    float metadata_width,
    float metadata_height,
    JerseyRoiMode mode,
    const Pose* pose) noexcept {
  JerseyCropSelection result;
  if (image.width < 1 || image.height < 1 || image.width > 65536 || image.height > 65536 ||
      !Coordinate(metadata_width) || !Coordinate(metadata_height) || metadata_width <= 0 || metadata_height <= 0 ||
      !Coordinate(box.left) || !Coordinate(box.top) || !Coordinate(box.width) || !Coordinate(box.height) ||
      box.width <= 0 || box.height <= 0)
    return result;
  double left, top, right, bottom;
  if (mode == JerseyRoiMode::kBox) {
    left = box.left + box.width * 0.2;
    right = box.left + box.width * 0.8;
    top = box.top + box.height * 0.25;
    bottom = box.top + box.height * 0.95;
  } else if (mode == JerseyRoiMode::kPose) {
    result.reason = JerseyCropReason::kInsufficientPose;
    if (!pose)
      return result;
    left = top = std::numeric_limits<double>::infinity();
    right = bottom = -std::numeric_limits<double>::infinity();
    for (size_t joint : {5U, 6U, 11U, 12U}) {
      const auto& point = (*pose)[joint];
      if (!Coordinate(point.x) || !Coordinate(point.y) || !std::isfinite(point.confidence) || point.confidence < 0.4F ||
          point.confidence > 1)
        return result;
      left = std::min(left, static_cast<double>(point.x));
      right = std::max(right, static_cast<double>(point.x));
      top = std::min(top, static_cast<double>(point.y));
      bottom = std::max(bottom, static_cast<double>(point.y));
    }
    if (right <= left || bottom <= top)
      return result;
    const double padding_x = (right - left) * 0.05, padding_y = (bottom - top) * 0.05;
    left -= padding_x;
    right += padding_x;
    top -= padding_y;
    bottom += padding_y;
  } else {
    return result;
  }
  result.reason = JerseyCropReason::kInvalidGeometry;
  const double sx = image.width / static_cast<double>(metadata_width);
  const double sy = image.height / static_cast<double>(metadata_height);
  left = std::floor(left * sx);
  top = std::floor(top * sy);
  right = std::ceil(right * sx);
  bottom = std::ceil(bottom * sy);
  if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom) || right <= left ||
      bottom <= top || std::max({std::abs(left), std::abs(top), std::abs(right), std::abs(bottom)}) > 1000000)
    return result;
  if (right <= 0 || bottom <= 0 || left >= image.width || top >= image.height) {
    result.reason = JerseyCropReason::kOutsideImage;
    return result;
  }
  if (right - left > kJerseyMaximumCropExtent || bottom - top > kJerseyMaximumCropExtent) {
    result.reason = JerseyCropReason::kTooLarge;
    return result;
  }
  result.crop = {
      static_cast<int>(left), static_cast<int>(top), static_cast<int>(right - left), static_cast<int>(bottom - top)};
  result.reason = JerseyCropReason::kReady;
  return result;
}

absl::StatusOr<JerseyVocabulary> MakeJerseyVocabulary(const ModelManifest& manifest) {
  if (manifest.feature != ModelFeature::kJersey || manifest.charset.size() != kJerseyTokens ||
      manifest.eos_index >= kJerseyTokens || manifest.charset[manifest.eos_index] != "[E]")
    return absl::InvalidArgumentError("jersey vocabulary does not match the prepared PARSeq contract");
  JerseyVocabulary result;
  std::fill(std::begin(result.digits), std::end(result.digits), -1);
  result.eos_index = manifest.eos_index;
  bool seen[10]{};
  for (size_t token = 0; token < manifest.charset.size(); ++token) {
    const auto& text = manifest.charset[token];
    if (text.size() == 1 && text[0] >= '0' && text[0] <= '9') {
      const int digit = text[0] - '0';
      if (seen[digit])
        return absl::InvalidArgumentError("duplicate jersey digit token");
      seen[digit] = true;
      result.digits[token] = digit;
    }
  }
  if (std::find(std::begin(seen), std::end(seen), false) != std::end(seen))
    return absl::InvalidArgumentError("jersey vocabulary is missing a digit token");
  return result;
}
} // namespace hm::player_analytics
