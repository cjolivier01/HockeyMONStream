#include "hstream/src/libs/stitching/ControlPointMatcher.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "absl/status/status.h"

namespace hm::stitching {

ControlPointResolution DefaultControlPointResolution() {
  return ControlPointResolution::k2K;
}

const char* ControlPointResolutionName(ControlPointResolution resolution) {
  if (resolution == ControlPointResolution::k1K)
    return "1k";
  return resolution == ControlPointResolution::k2K ? "2k" : "native";
}

absl::StatusOr<ControlPointResolution> ParseControlPointResolution(const std::string& value) {
  if (value == "auto")
    return DefaultControlPointResolution();
  if (value == "native")
    return ControlPointResolution::kNative;
  if (value == "2k")
    return ControlPointResolution::k2K;
  if (value == "1k")
    return ControlPointResolution::k1K;
  return absl::InvalidArgumentError("stitching.control_point_resolution must be auto, native, 1k or 2k: " + value);
}

const char* ControlPointMatcherName(ControlPointMatcher matcher) {
  switch (matcher) {
    case ControlPointMatcher::kSuperPointLightGlue:
      return "superpoint-lightglue";
    case ControlPointMatcher::kDeDoDeLightGlue:
      return "dedode-lightglue";
    case ControlPointMatcher::kLoFTR:
      return "loftr";
    case ControlPointMatcher::kAkazeHamming:
      return "akaze-hamming";
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
  if (normalized == "akaze-hamming" || normalized == "akaze" || normalized == "akaze-mldb" ||
      normalized == "akaze-mldb-hamming") {
    return ControlPointMatcher::kAkazeHamming;
  }
  return absl::InvalidArgumentError(
      "Unsupported native control-point matcher \"" + value +
      "\"; choose superpoint-lightglue, dedode-lightglue, loftr, or akaze-hamming");
}

} // namespace hm::stitching
