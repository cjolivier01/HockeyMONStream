#pragma once

#include <string>

#include "absl/status/statusor.h"

namespace hm::stitching {

enum class ControlPointMatcher {
  kSuperPointLightGlue,
  kDeDoDeLightGlue,
  kLoFTR,
};

const char* ControlPointMatcherName(ControlPointMatcher matcher);
absl::StatusOr<ControlPointMatcher> ParseControlPointMatcher(const std::string& value);

} // namespace hm::stitching
