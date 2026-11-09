#include "hstream/src/libs/detection/TileInference.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace hm::detection {
absl::Status ValidateTileInference(const TileInferenceSettings& settings) {
  if (settings.tiles != 2 && settings.tiles != 4)
    return absl::InvalidArgumentError("Tiled inference requires tiles=2 (halves) or tiles=4 (quadrants)");
  if (!std::isfinite(settings.overlap) || settings.overlap < 0 || settings.overlap >= 1 ||
      !std::isfinite(settings.merge_iou) || settings.merge_iou <= 0 || settings.merge_iou > 1)
    return absl::InvalidArgumentError("Tiled inference requires overlap in [0,1) and merge-iou in (0,1]");
  return absl::OkStatus();
}

absl::StatusOr<std::vector<DetectionTile>> MakeDetectionTiles(
    int width,
    int height,
    const TileInferenceSettings& settings) {
  const auto status = ValidateTileInference(settings);
  if (!status.ok())
    return status;
  if (width < 4 || height < (settings.tiles == 4 ? 4 : 2) || width >= (1 << 24) || height >= (1 << 24))
    return absl::InvalidArgumentError("Invalid tiled detector canvas or overlap");
  width &= ~1;
  height &= ~1;
  const auto extent = [&](int size) {
    return std::min(size, 2 * static_cast<int>(std::ceil(size / (2.0 - settings.overlap) / 2.0)));
  };
  const int crop_width = extent(width);
  const int crop_height = settings.tiles == 4 ? extent(height) : height;
  std::vector<DetectionTile> tiles{{0, 0, crop_width, crop_height}, {width - crop_width, 0, crop_width, crop_height}};
  if (settings.tiles == 4) {
    tiles.push_back({0, height - crop_height, crop_width, crop_height});
    tiles.push_back({width - crop_width, height - crop_height, crop_width, crop_height});
  }
  return tiles;
}

absl::StatusOr<std::vector<size_t>> MergeTileProposals(
    const std::vector<TileProposal>& proposals,
    const TileInferenceSettings& settings) {
  const auto status = ValidateTileInference(settings);
  if (!status.ok())
    return status;
  for (const auto& entry : proposals) {
    const auto& p = entry.box;
    if (entry.tile >= static_cast<unsigned>(settings.tiles) || p.class_id < 0 || !std::isfinite(p.left) ||
        !std::isfinite(p.top) || !std::isfinite(p.right) || !std::isfinite(p.bottom) || !std::isfinite(p.confidence) ||
        p.confidence < 0 || p.confidence > 1 || p.left < 0 || p.top < 0 || p.right <= p.left || p.bottom <= p.top)
      return absl::InvalidArgumentError("Invalid tile detection");
  }
  std::vector<size_t> order(proposals.size()), selected;
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return proposals[a].box.confidence > proposals[b].box.confidence;
  });
  for (size_t index : order) {
    const auto& p = proposals[index];
    bool duplicate = false;
    for (size_t previous : selected) {
      const auto& q = proposals[previous];
      if (p.tile == q.tile || p.box.class_id != q.box.class_id)
        continue;
      const double intersection = std::max(0.f, std::min(p.box.right, q.box.right) - std::max(p.box.left, q.box.left)) *
          static_cast<double>(std::max(0.f, std::min(p.box.bottom, q.box.bottom) - std::max(p.box.top, q.box.top)));
      const double area = (p.box.right - p.box.left) * static_cast<double>(p.box.bottom - p.box.top) +
          (q.box.right - q.box.left) * static_cast<double>(q.box.bottom - q.box.top) - intersection;
      if (intersection / area > settings.merge_iou) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate)
      selected.push_back(index);
  }
  return selected;
}
} // namespace hm::detection
