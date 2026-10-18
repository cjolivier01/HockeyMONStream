#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hstream/src/libs/stitching/RinkMap.h"

namespace hm::stitching {

// Immutable startup snapshot. The grid contains calibrated viewing rays, not
// image pixels. Rendering only reads this data and frame metadata.
struct PlaybackRinkMap {
  RinkLayout layout;
  RinkMapFit fit;
  std::string hugin_generation;
  size_t columns{257}, rows{129};
  std::vector<cv::Vec3d> rays;

  // Accept only the matching Hugin generation and exact live output size.
  // The returned angle is the frame's post-stitch rotation, including live edits.
  std::optional<double> OutputRotation(const std::string& generation, unsigned width, unsigned height) const;
  // Normalized box-foot coordinates in the full stitched frame, before Program
  // cropping. Invalid rays, points outside the ice and stale maps yield no dot.
  std::optional<cv::Point2d> Locate(double x, double y, double rotation, unsigned width, unsigned height) const;
};

// Shared with the editor; preserves the original revision format.
absl::StatusOr<std::string> RinkMapCalibrationRevision(const std::filesystem::path& directory);

// Bounded startup work, before preview activation. No live video is read back.
// Recomputes saved observation rays and fits using the current NONA project,
// then prepares the output-projection grid with the installed Hugin tool.
absl::StatusOr<std::shared_ptr<const PlaybackRinkMap>> LoadPlaybackRinkMap(
    const std::filesystem::path& directory,
    const std::function<bool()>& cancelled = {});

// Converts a projection-only grid using the same Hugin implementation as NONA.
// Exposed for projection/crop regression checks; source image files are unused.
absl::StatusOr<std::vector<cv::Vec3d>> RinkProjectionRays(
    const std::string& pto,
    size_t crop_width,
    size_t crop_height,
    size_t columns,
    size_t rows,
    const std::function<bool()>& cancelled = {});

} // namespace hm::stitching
