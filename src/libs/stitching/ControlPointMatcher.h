#pragma once

#include <string>

#include "absl/status/statusor.h"

namespace hm::stitching {

// Shared UI budget and general calibration floor. AKAZE with OpenCV retains
// its specialized six-match floor; geometric validation can still reject a set.
inline constexpr int kMinimumCalibrationControlPoints = 10;

enum class ControlPointMatcher {
  kSuperPointLightGlue,
  kDeDoDeLightGlue,
  kLoFTR,
  kAkazeHamming,
};

// SuperPoint has a dynamic graph; the other backends retain their established sizes.
enum class ControlPointResolution { kNative, k2K, k1K };
// Missing settings and the legacy auto value resolve to 2K on every platform.
ControlPointResolution DefaultControlPointResolution();
const char* ControlPointResolutionName(ControlPointResolution resolution);
absl::StatusOr<ControlPointResolution> ParseControlPointResolution(const std::string& value);

const char* ControlPointMatcherName(ControlPointMatcher matcher);
absl::StatusOr<ControlPointMatcher> ParseControlPointMatcher(const std::string& value);

} // namespace hm::stitching
