#pragma once

#include <optional>

#include "hstream/src/libs/common/ConfigLayers.h"
#include "hstream/src/libs/scoreboard/ScoreboardPlan.h"

namespace hm::program {
struct ProgramScoreboardConfig {
  std::array<std::array<float, 2>, 4> polygon;
  std::string generation;
  int requested_width, requested_height;
  scoreboard::Position position;
};
// Resolve saved polygon, dimensions and placement without opening GPU state.
// Disabled and all-zero sentinel polygons return no overlay. An enabled overlay
// requires a saved polygon and its output-generation identity; interactive
// selection is still owned by the NVIDIA/desktop path.
absl::StatusOr<std::optional<ProgramScoreboardConfig>> ResolveProgramScoreboardConfig(
    const config::ProgramConfiguration& config,
    int output_width,
    int output_height);
absl::StatusOr<scoreboard::Plan> PrepareProgramScoreboard(
    const ProgramScoreboardConfig& config,
    const std::string& generation,
    int source_width,
    int source_height,
    int output_width,
    int output_height);
} // namespace hm::program
