#pragma once

/* clang-format off */
#include "src/libs/common/Status.h"
/* clang-format on */

#include <functional>
#include <string>
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

absl::StatusOr<Synchronization> calculate_stitching_synchronization(
    const std::string& video1,
    const std::string& video2);

absl::StatusOr<bool> is_stitching_configured(const std::string& game_dir);

absl::StatusOr<bool> stitching_artifacts_exceed_live_canvas_limit(const std::string& game_dir);

bool can_configure_stitching(const YAML::Node& config);

absl::StatusOr<std::string> stitched_output_generation_id(
    const std::string& hugin_generation,
    double post_stitch_rotate_degrees);

bool is_field_mask_configured(const std::string& game_dir, const std::string& expected_output_generation = {});

// Validates and decodes rink_mask_0.png while holding the Hugin and
// config/rink transaction locks for one complete artifact generation.
absl::StatusOr<cv::Mat> load_field_mask(
    const std::string& game_dir,
    const std::string& expected_output_generation = {});

absl::Status create_field_mask(
    const std::string& game_dir,
    surface::Surface surface,
    const std::string& expected_output_generation = {},
    const std::string& expected_invalidation_id = {});

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

absl::Status configure_orientation(const std::string& game_dir, const std::string& expected_invalidation_id = {});

bool is_scoreboard_configured(const std::string& game_dir);

absl::Status configure_scoreboard(const std::string& game_dir);

absl::Status configure_stitching(
    const std::string& game_dir,
    surface::Surface left_surface,
    surface::Surface right_surface,
    const std::string& expected_invalidation_id = {},
    const std::function<bool()>& is_cancelled = {});

// Ensure `${game_dir}/seam_file.png` exists.
//
// Some environments produce the mapping TIFFs but fail to generate `seam_file.png` (e.g. missing enblend/multiblend).
// The runtime stitcher requires a seam mask; without it the pipeline will render a gray canvas.
//
// When missing, this creates a simple "hard seam" mask based on the mapping TIFF placements.
// It is intended as a robust fallback for debugging; higher-quality seams can still be generated offline.
absl::Status maybe_create_default_seam_file(const std::string& game_dir);

} // namespace stitching
} // namespace hm
