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

  struct CanvasSnapshot {
    int width{0};
    int height{0};
    std::string png_bytes;
    CanvasGeneration generation;
  };

  // The immutable bytes and identity are captured together under native locks.
  static absl::StatusOr<CanvasSnapshot> OpenCanvas(
      const std::filesystem::path& game_dir,
      bool allow_single_source = false,
      const std::function<bool()>& is_cancelled = {});

  // Compare source ownership independently from stitched artifacts.
  static absl::StatusOr<bool> SameSingleSource(
      const std::filesystem::path& game_dir,
      const std::filesystem::path& reference_dir);

  static bool IsDisabled(const Polygon& polygon);
  static absl::StatusOr<Polygon> ValidateAndOrder(Polygon polygon, int image_width, int image_height);
  static absl::Status Save(
      const std::filesystem::path& game_dir,
      const Polygon& polygon,
      const std::optional<CanvasGeneration>& expected_generation = std::nullopt,
      const std::filesystem::path& single_source_reference = {},
      const std::function<bool()>& is_cancelled = {});
  // Wait for the operator, but release the video worker promptly on pipeline stop.
  static absl::Status Run(const std::filesystem::path& game_dir, const std::function<bool()>& is_cancelled = {});
};

} // namespace hm::stitching
