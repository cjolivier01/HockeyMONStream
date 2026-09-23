#pragma once

#include <array>

#include "hstream/src/libs/stitching/FeatureMatcher.h"

namespace hm::stitching {

// Diagnostic stills only: each camera is bounded to 1024 pixels per edge.
// Returns points and points-with-match-lines, using the matcher-selected pairs
// before multi-frame pooling/geometric validation. No inference or video readback.
absl::StatusOr<std::array<cv::Mat, 2>> MakeCalibrationMatchImages(
    const std::array<cv::Mat, 2>& images,
    const std::vector<FeatureMatch>& selected,
    const AkazeMatchingCalibration& calibration = {});

} // namespace hm::stitching
