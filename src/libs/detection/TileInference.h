#pragma once

#include <array>
#include <vector>

#include "absl/status/statusor.h"
#include "hstream/src/libs/detection/DetectionTypes.h"

namespace hm::detection {

struct TileInferenceSettings {
  double overlap;
  float merge_iou;
};

struct TileProposal {
  Proposal box; // Original stitched-image coordinates.
  unsigned tile;
};

// Two full-height crops, with overlap expressed as a fraction of each crop's
// width. Even alignment matches nvinfer's native crop transform. An odd final
// row/column is excluded, just as in its ordinary full-frame preprocessing.
absl::StatusOr<std::array<DetectionTile, 2>> MakeDetectionTiles(int width, int height, double overlap);
absl::Status ValidateTileInference(const TileInferenceSettings& settings);

// Return indices into the input, in deterministic confidence order. Suppress
// only same-class duplicates from different tiles; per-tile NMS already ran.
absl::StatusOr<std::vector<size_t>> MergeTileProposals(const std::vector<TileProposal>& proposals, float iou);

} // namespace hm::detection
