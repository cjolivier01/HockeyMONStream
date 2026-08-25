#pragma once

#include "hstream/src/libs/stitching/FieldMaskArtifact.h"
#include "hstream/src/libs/stitching/HuginProject.h"

/* clang-format off */
#include "src/libs/common/Status.h"
/* clang-format on */

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "hstream/src/libs/common/Surface.h"
#include "opencv2/core/mat.hpp"
#include "yaml-cpp/node/node.h"

namespace hm {
namespace stitching {

struct RinkProfile;

struct Synchronization {
  // Actual frame number
  double video1_frame_offset{0};
  // Actual frame number
  double video2_frame_offset{0};
};

struct StitchingCanvasSize {
  size_t width{0};
  size_t height{0};
};

struct StitchingCalibrationFramePair {
  surface::Surface left;
  surface::Surface right;
};

absl::StatusOr<Synchronization> calculate_stitching_synchronization(
    const std::string& video1,
    const std::string& video2);

absl::StatusOr<bool> is_stitching_configured(const std::string& game_dir, size_t max_output_width = 0);

// Validates one artifact generation, normalizes its seam, and retains the
// publication lock so the caller can load the same generation atomically.
// A null lock denotes a valid game directory without configured artifacts.
absl::StatusOr<std::unique_ptr<HuginProject::ArtifactLock>> lock_validated_stitching_artifacts(
    const std::string& game_dir,
    size_t max_output_width = 0);

absl::StatusOr<StitchingCanvasSize> stitching_canvas_size(const std::string& game_dir, size_t max_output_width = 0);

absl::StatusOr<bool> stitching_artifacts_exceed_live_canvas_limit(
    const std::string& game_dir,
    size_t max_output_width = 0);

absl::StatusOr<bool> stitching_artifacts_require_canvas_regeneration(
    const std::string& game_dir,
    size_t max_output_width = 0);

bool can_configure_stitching(const YAML::Node& config);

absl::StatusOr<std::string> stitched_output_generation_id(
    const std::string& hugin_generation,
    double post_stitch_rotate_degrees,
    size_t output_width = 0,
    size_t output_height = 0);

absl::StatusOr<std::string> configured_stitched_output_generation_id(
    const std::string& game_dir,
    size_t max_output_width = 0);

// Validates that a completion event describes the current Hugin artifacts and
// calibration owner without requiring a downstream rink mask. The rotation in
// the reported generation is the authoritative live hmstitcher value.
absl::Status validate_stitched_output_generation(
    const std::string& game_dir,
    const std::string& expected_output_generation,
    const std::string& expected_invalidation_id = {});

bool is_field_mask_configured(
    const std::string& game_dir,
    const std::string& expected_output_generation = {},
    const std::string& expected_invalidation_id = {});

bool is_field_mask_configured_for_stitching_config(
    const std::string& game_dir,
    size_t max_output_width = 0,
    const std::string& expected_invalidation_id = {});

// Validates and decodes rink_mask_0.png while holding the Hugin and
// config/rink transaction locks for one complete artifact generation.
absl::StatusOr<cv::Mat> load_field_mask(
    const std::string& game_dir,
    const std::string& expected_output_generation = {},
    const std::string& expected_invalidation_id = {});

absl::Status create_field_mask(
    const std::string& game_dir,
    surface::Surface surface,
    const std::string& expected_output_generation = {},
    const std::string& expected_invalidation_id = {},
    const std::function<bool()>& is_cancelled = {});

absl::Status save_rink_profile(
    const std::string& game_dir,
    const RinkProfile& profile,
    const std::string& expected_invalidation_id = {});

// Atomically publishes the stitched calibration snapshot together with the
// rink profile after revalidating its generation owner.
absl::Status save_rink_profile_with_stitched_image(
    const std::string& game_dir,
    const RinkProfile& profile,
    const cv::Mat& stitched_image,
    const std::string& expected_invalidation_id = {});

absl::Status save_stitched_image(const std::string& game_dir, surface::Surface surface);

// Revalidates a pending UI invalidation under the game-config lock and
// returns whether its artifact cleanup has already been applied.
absl::StatusOr<bool> is_stitching_invalidation_cleanup_applied(
    const std::string& game_dir,
    const std::string& expected_invalidation_id);

absl::Status clean_stitching_artifacts(const std::string& game_dir, const std::string& expected_invalidation_id = {});

// Invalidates control-point generation and every artifact that depends on it,
// while preserving camera orientation and synchronization results.
absl::Status clean_stitching_artifacts_from_control_points(
    const std::string& game_dir,
    const std::string& expected_invalidation_id = {});

absl::Status configure_orientation(
    const std::string& game_dir,
    const std::string& expected_invalidation_id = {},
    const std::function<bool()>& is_cancelled = {});

bool is_scoreboard_configured(const std::string& game_dir);

absl::Status configure_scoreboard(const std::string& game_dir);

absl::Status configure_stitching(
    const std::string& game_dir,
    surface::Surface left_surface,
    surface::Surface right_surface,
    const std::string& expected_invalidation_id = {},
    const std::function<bool()>& is_cancelled = {},
    size_t max_output_width = 0);

absl::Status configure_stitching(
    const std::string& game_dir,
    const std::vector<StitchingCalibrationFramePair>& frame_pairs,
    const std::string& expected_invalidation_id = {},
    const std::function<bool()>& is_cancelled = {},
    size_t max_output_width = 0);

// Validate that `${game_dir}/seam_file.png` exists.
//
// Some environments produce the mapping TIFFs but fail to generate `seam_file.png` (e.g. missing enblend/multiblend).
// The runtime stitcher requires a seam mask; without it the pipeline will render a gray canvas.
//
// With HM_ALLOW_HARD_SEAM_FALLBACK=1, a missing or invalid seam is replaced by a simple "hard seam" mask based on the
// mapping TIFF placements. Without that explicit diagnostic opt-in, a missing or invalid seam fails closed.
absl::Status maybe_create_default_seam_file(const std::string& game_dir, size_t max_output_width = 0);

} // namespace stitching
} // namespace hm
