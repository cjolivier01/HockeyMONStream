#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>

#include <opencv2/core.hpp>

#include "absl/status/statusor.h"

namespace hm::stitching {

struct PlayerFrameOverlap {
  cv::Mat mask; // CV_8UC1, bounded resolution; nonzero means shared camera visibility.
  cv::Size canvas_size;
  std::string generation_id;
  std::string artifact_revision;
};

struct PlayerFrameRemap {
  cv::Mat x; // CV_16UC1; 65535 is unmapped.
  cv::Mat y;
  cv::Point2d placement;
  cv::Size source_size;
};

// Pure geometry helper. Placements are normalized by their common minimum;
// validity is reduced conservatively (holes cannot disappear during reduction).
// effective_canvas must have the same aspect ratio, within integer rounding.
absl::StatusOr<PlayerFrameOverlap> BuildPlayerFrameOverlap(
    const std::array<PlayerFrameRemap, 2>& maps,
    cv::Size effective_canvas,
    double post_stitch_rotation_degrees,
    size_t maximum_mask_dimension = 1024);

// Acquires an existing validated snapshot, streams TIFF remaps into bounded
// masks, then checks snapshot stability. Never creates or repairs calibration.
// Current production mappings are generated at the effective capped size;
// max_output_width validates that cap rather than resizing stale mappings.
absl::StatusOr<PlayerFrameOverlap> LoadPlayerFrameOverlap(
    const std::filesystem::path& game_directory,
    const std::array<cv::Size, 2>& source_sizes,
    size_t max_output_width,
    double post_stitch_rotation_degrees,
    size_t maximum_mask_dimension = 1024);

} // namespace hm::stitching
