#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "hstream/src/libs/stitching/FeatureMatcher.h"

namespace hm::stitching {

class HuginProject {
 public:
  struct Options {
    double horizontal_fov{108.0};
    std::optional<size_t> max_canvas_dimension;
  };

  // Pure helpers exposed for focused contract tests.
  static absl::StatusOr<std::string> InsertControlPoints(
      const std::string& pto,
      const std::vector<FeatureMatch>& matches);
  static absl::StatusOr<std::pair<size_t, size_t>> ParseCanvasSize(const std::string& pto);

  // Builds all Hugin products in a private same-filesystem directory and only
  // publishes them into game_dir after every required mapping has validated.
  static absl::Status Configure(
      const std::filesystem::path& game_dir,
      const std::vector<FeatureMatch>& matches,
      const Options& options);
};

} // namespace hm::stitching
