#pragma once

#include <vector>

#include "absl/status/statusor.h"
#include "hstream/src/libs/detection/DetectionTypes.h"

namespace hm::detection {

struct TileInferenceSettings {
  constexpr TileInferenceSettings(double overlap, float merge_iou, int tiles)
      : overlap(overlap), merge_iou(merge_iou), tiles(tiles) {}

  double overlap;
  float merge_iou;
  int tiles;
};

struct TileProposal {
  Proposal box; // Original stitched-image coordinates.
  unsigned tile;
};

// Two full-height halves or four quadrants. Overlap is a fraction of the crop
// extent along each split axis. Even alignment matches nvinfer's native crop transform. An odd final
// row/column is excluded, just as in its ordinary full-frame preprocessing.
absl::StatusOr<std::vector<DetectionTile>> MakeDetectionTiles(
    int width,
    int height,
    const TileInferenceSettings& settings);
absl::Status ValidateTileInference(const TileInferenceSettings& settings);

// Return indices into the input, in deterministic confidence order. Suppress
// only same-class duplicates from different tiles; per-tile NMS already ran.
absl::StatusOr<std::vector<size_t>> MergeTileProposals(
    const std::vector<TileProposal>& proposals,
    const TileInferenceSettings& settings);

} // namespace hm::detection
