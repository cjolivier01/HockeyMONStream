#pragma once

/* clang-format off */
#include "src/libs/common/Status.h"
/* clang-format on */

#include <string>
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "hstream/src/libs/common/Surface.h"
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

bool is_field_mask_configured(const std::string& game_dir);

absl::Status create_field_mask(const std::string& game_dir, surface::Surface surface);

absl::Status save_rink_profile(const std::string& game_dir, const RinkProfile& profile);

absl::Status save_stitched_image(const std::string& game_dir, surface::Surface surface);

absl::Status clean_stitching_artifacts(const std::string& game_dir);

absl::Status configure_orientation(const std::string& game_dir);

bool is_scoreboard_configured(const std::string& game_dir);

absl::Status configure_scoreboard(const std::string& game_dir);

absl::Status configure_stitching(
    const std::string& game_dir,
    surface::Surface left_surface,
    surface::Surface right_surface);

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
