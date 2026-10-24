#pragma once

#include "hstream/src/libs/common/Surface.h"
#include "hstream/src/libs/stitching/ConfigureStitchingCore.h"

namespace hm::stitching {
struct StitchingCalibrationFramePair {
  surface::Surface left;
  surface::Surface right;
  CalibrationFrameSource left_source;
  CalibrationFrameSource right_source;
  // Validated immutable PNGs replace GPU surfaces when replaying a saved set.
  // Both paths must be supplied together; no upload/readback is needed.
  std::filesystem::path left_image;
  std::filesystem::path right_image;
};

absl::Status create_field_mask(
    const std::string& game_dir,
    surface::Surface surface,
    const std::string& expected_output_generation = {},
    const std::string& expected_invalidation_id = {},
    const std::function<bool()>& is_cancelled = {},
    const std::string& expected_output_authorization_id = {},
    const RinkMaskFrameProvenance& frame = {});

absl::Status save_stitched_image(
    const std::string& game_dir,
    surface::Surface surface,
    const std::string& producer_output_generation);

absl::Status configure_stitching(
    const std::string& game_dir,
    surface::Surface left_surface,
    surface::Surface right_surface,
    const std::string& expected_invalidation_id = {},
    const std::function<bool()>& is_cancelled = {},
    size_t max_output_width = 0,
    const std::string& captured_frame_selection_fingerprint = {});

absl::Status configure_stitching(
    const std::string& game_dir,
    const std::vector<StitchingCalibrationFramePair>& frame_pairs,
    const std::string& expected_invalidation_id = {},
    const std::function<bool()>& is_cancelled = {},
    size_t max_output_width = 0,
    const std::string& captured_frame_selection_fingerprint = {});

} // namespace hm::stitching
