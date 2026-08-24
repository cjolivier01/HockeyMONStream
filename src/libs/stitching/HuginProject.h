#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "hstream/src/libs/stitching/FeatureMatcher.h"
#include "hstream/src/libs/stitching/HomographyMaps.h"

namespace hm::stitching {

// Hard-seam generation is a diagnostic fallback and must be explicitly opted
// into with HM_ALLOW_HARD_SEAM_FALLBACK=1.
bool hard_seam_fallback_enabled();

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
    using ProgressCallback =
        std::function<void(const std::string& stage, const std::string& status, const std::string& message)>;

    double horizontal_fov{108.0};
    std::optional<size_t> max_canvas_dimension;
    MappingBackend mapping_backend{MappingBackend::kNona};
    std::string expected_invalidation_id;
    ProgressCallback progress;
    std::function<bool()> is_cancelled;
  };

  // Pure helpers exposed for focused contract tests.
  static absl::StatusOr<std::string> InsertControlPoints(
      const std::string& pto,
      const std::vector<FeatureMatch>& matches);
  static absl::StatusOr<std::pair<size_t, size_t>> ParseCanvasSize(const std::string& pto);
  static absl::StatusOr<int> ParseProjection(const std::string& pto);
  static absl::StatusOr<CameraPose> ParseCameraPose(const std::string& pto, size_t image_index);

  // Validate a two-camera enblend seam and expand any pixel-offset crop to the
  // full mapping canvas expected by hm-cupano.
  static absl::Status ValidateAndNormalizeSeam(
      const std::filesystem::path& seam_path,
      int canvas_width,
      int canvas_height);
  static absl::Status ValidateAndNormalizeSeam(
      const std::filesystem::path& seam_path,
      int native_canvas_width,
      int native_canvas_height,
      int effective_canvas_width,
      int effective_canvas_height);

  // Builds all Hugin products in a private same-filesystem directory and only
  // publishes them into game_dir after every required mapping has validated.
  static absl::Status Configure(
      const std::filesystem::path& game_dir,
      const std::vector<FeatureMatch>& matches,
      const Options& options);

  // As above, but consume immutable, run-private synchronized images. The
  // images are copied into and published with the locked Hugin generation, so
  // concurrent calibrations cannot pair one run's matches with another run's
  // public left.png/right.png files.
  static absl::Status Configure(
      const std::filesystem::path& game_dir,
      const std::filesystem::path& left_image,
      const std::filesystem::path& right_image,
      const std::vector<FeatureMatch>& matches,
      const Options& options);

  // Recover an interrupted durable publication before opening the flat Hugin
  // artifact set from game_dir.
  static absl::Status Recover(const std::filesystem::path& game_dir);

  // Recover and retain the publication lock while a caller decodes the flat
  // artifact set. This prevents a concurrent calibration from exposing a
  // mixed generation to ControlMasks readers.
  static absl::StatusOr<std::unique_ptr<ArtifactLock>> RecoverAndLock(const std::filesystem::path& game_dir);

  // Identifies the currently published flat Hugin generation. The supplied
  // lock must still be held so every artifact belongs to one generation.
  static absl::StatusOr<std::string> GenerationId(const std::filesystem::path& game_dir, const ArtifactLock& lock);
};

} // namespace hm::stitching
