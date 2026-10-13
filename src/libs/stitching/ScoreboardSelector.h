#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::stitching {

struct ScoreboardPoint {
  int x{0};
  int y{0};
};

class ScoreboardSelector {
 public:
  using Polygon = std::array<ScoreboardPoint, 4>;

  struct CanvasGeneration {
    std::string hugin_generation;
    std::string stitched_output_generation;
    std::string snapshot_identity;
  };

  static bool IsDisabled(const Polygon& polygon);
  static absl::StatusOr<Polygon> ValidateAndOrder(Polygon polygon, int image_width, int image_height);
  static absl::Status Save(
      const std::filesystem::path& game_dir,
      const Polygon& polygon,
      const std::optional<CanvasGeneration>& expected_generation = std::nullopt);
  // Wait for the operator, but release the video worker promptly on pipeline stop.
  static absl::Status Run(const std::filesystem::path& game_dir, const std::function<bool()>& is_cancelled = {});
};

} // namespace hm::stitching
