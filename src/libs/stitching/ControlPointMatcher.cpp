#include "hstream/src/libs/stitching/ControlPointMatcher.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "absl/status/status.h"

namespace hm::stitching {

const char* ControlPointMatcherName(ControlPointMatcher matcher) {
  switch (matcher) {
    case ControlPointMatcher::kSuperPointLightGlue:
      return "superpoint-lightglue";
    case ControlPointMatcher::kDeDoDeLightGlue:
      return "dedode-lightglue";
    case ControlPointMatcher::kLoFTR:
      return "loftr";
  }
  return "superpoint-lightglue";
}

absl::StatusOr<ControlPointMatcher> ParseControlPointMatcher(const std::string& value) {
  std::string normalized = value.empty() ? "superpoint-lightglue" : value;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
    return character == '_' ? '-' : static_cast<char>(std::tolower(character));
  });
  if (normalized == "aliked-lightglue" || normalized == "raco-aliked-lightglue" ||
      normalized == "native-aliked-lightglue" || normalized == "superpoint-lightglue" || normalized == "superpoint" ||
      normalized == "lightglue") {
    return ControlPointMatcher::kSuperPointLightGlue;
  }
  if (normalized == "dedode-lightglue" || normalized == "dedode") {
    return ControlPointMatcher::kDeDoDeLightGlue;
  }
  if (normalized == "loftr") {
    return ControlPointMatcher::kLoFTR;
  }
  return absl::InvalidArgumentError(
      "Unsupported native control-point matcher \"" + value +
      "\"; choose superpoint-lightglue, dedode-lightglue, or loftr");
}

} // namespace hm::stitching
