#pragma once

#include <array>
#include <filesystem>
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

  static bool IsDisabled(const Polygon& polygon);
  static absl::StatusOr<Polygon> ValidateAndOrder(Polygon polygon, int image_width, int image_height);
  static absl::Status Save(
      const std::filesystem::path& game_dir,
      const Polygon& polygon,
      const std::string& expected_hugin_generation = {});
  static absl::Status Run(const std::filesystem::path& game_dir);
};

} // namespace hm::stitching
