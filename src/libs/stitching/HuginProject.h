#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
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
  class ArtifactLock {
   public:
    ~ArtifactLock();
    ArtifactLock(const ArtifactLock&) = delete;
    ArtifactLock& operator=(const ArtifactLock&) = delete;

   private:
    friend class HuginProject;
    explicit ArtifactLock(int descriptor) : descriptor_(descriptor) {}
    int descriptor_{-1};
  };

  struct CameraPose {
    double roll;
    double pitch;
    double yaw;
  };

  struct Options {
    double horizontal_fov{108.0};
    std::optional<size_t> max_canvas_dimension;
  };

  // Pure helpers exposed for focused contract tests.
  static absl::StatusOr<std::string> InsertControlPoints(
      const std::string& pto,
      const std::vector<FeatureMatch>& matches);
  static absl::StatusOr<std::pair<size_t, size_t>> ParseCanvasSize(const std::string& pto);
  static absl::StatusOr<int> ParseProjection(const std::string& pto);
  static absl::StatusOr<CameraPose> ParseCameraPose(const std::string& pto, size_t image_index);

  // Builds all Hugin products in a private same-filesystem directory and only
  // publishes them into game_dir after every required mapping has validated.
  static absl::Status Configure(
      const std::filesystem::path& game_dir,
      const std::vector<FeatureMatch>& matches,
      const Options& options);

  // Recover an interrupted durable publication before opening the flat Hugin
  // artifact set from game_dir.
  static absl::Status Recover(const std::filesystem::path& game_dir);

  // Recover and retain the publication lock while a caller decodes the flat
  // artifact set. This prevents a concurrent calibration from exposing a
  // mixed generation to ControlMasks readers.
  static absl::StatusOr<std::unique_ptr<ArtifactLock>> RecoverAndLock(const std::filesystem::path& game_dir);
};

} // namespace hm::stitching
