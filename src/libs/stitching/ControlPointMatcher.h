#pragma once

#include <string>

#include "absl/status/statusor.h"

namespace hm::stitching {

// Shared UI budget and general calibration floor. AKAZE with OpenCV retains
// its specialized six-match floor; geometric validation can still reject a set.
inline constexpr int kMinimumCalibrationControlPoints = 10;

// Independent checks applied to automatic matches before spatial capping.
struct ControlPointFiltering {
  bool epipolar_ransac{true};
  bool homography_consistency{true};
  bool operator==(const ControlPointFiltering& other) const {
    return epipolar_ransac == other.epipolar_ransac && homography_consistency == other.homography_consistency;
  }
  bool operator!=(const ControlPointFiltering& other) const {
    return !(*this == other);
  }
};

enum class ControlPointMatcher {
  kSuperPointLightGlue,
  kDeDoDeLightGlue,
  kLoFTR,
  kAkazeHamming,
};

// Provenance predating configurable filters had epipolar filtering only for AKAZE.
inline ControlPointFiltering LegacyControlPointFiltering(ControlPointMatcher matcher) {
  return {matcher == ControlPointMatcher::kAkazeHamming, false};
}

// SuperPoint has a dynamic graph; the other backends retain their established sizes.
enum class ControlPointResolution { kNative, k2K, k1K };
// Missing settings and the legacy auto value resolve to 2K on every platform.
ControlPointResolution DefaultControlPointResolution();
const char* ControlPointResolutionName(ControlPointResolution resolution);
absl::StatusOr<ControlPointResolution> ParseControlPointResolution(const std::string& value);

const char* ControlPointMatcherName(ControlPointMatcher matcher);
absl::StatusOr<ControlPointMatcher> ParseControlPointMatcher(const std::string& value);

} // namespace hm::stitching
