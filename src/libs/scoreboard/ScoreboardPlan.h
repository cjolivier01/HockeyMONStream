#pragma once

#include <array>

#include "absl/status/statusor.h"
#include "hstream/src/libs/scoreboard/ScoreboardPlacement.h"

namespace hm::scoreboard {
// Immutable output-pixel to panorama-pixel mapping. Destination coordinates are
// relative to the overlay rectangle. Construct once for each output geometry.
// GPU consumers copy this small value; it owns no video pixels or GPU resource.
struct Plan {
  int source_width, source_height;
  int output_width, output_height;
  int left, top, width, height;
  float inverse[9];
};

// Points must be TL, TR, BR, BL in the current panorama's pixel coordinates.
// Match native Scoreboard's least-relative-change aspect adjustment. All sizing
// and placement choices are explicit; there are no application defaults here.
absl::StatusOr<Plan> MakePlan(
    const std::array<std::array<float, 2>, 4>& polygon,
    int source_width,
    int source_height,
    int output_width,
    int output_height,
    int requested_width,
    int requested_height,
    Position position);
bool ValidatePlan(const Plan& plan);
} // namespace hm::scoreboard
