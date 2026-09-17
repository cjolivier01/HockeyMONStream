#pragma once

#include <string>

#include "absl/status/statusor.h"

namespace hm::stitching {

enum class ControlPointMatcher {
  kSuperPointLightGlue,
  kDeDoDeLightGlue,
  kLoFTR,
  kAkazeHamming,
};

// SuperPoint has a dynamic graph; the other backends retain their established sizes.
enum class ControlPointResolution { kNative, k2K };
// The canonical auto setting resolves to 2K on Jetson and native on desktop/SBSA.
ControlPointResolution DefaultControlPointResolution();
const char* ControlPointResolutionName(ControlPointResolution resolution);
absl::StatusOr<ControlPointResolution> ParseControlPointResolution(const std::string& value);

const char* ControlPointMatcherName(ControlPointMatcher matcher);
absl::StatusOr<ControlPointMatcher> ParseControlPointMatcher(const std::string& value);

} // namespace hm::stitching
