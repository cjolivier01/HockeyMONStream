#pragma once

#include <filesystem>
#include <string>

#include "absl/status/statusor.h"
#include "hstream/src/libs/stitching/ControlPointMatcher.h"

namespace hm::stitching {

absl::StatusOr<std::filesystem::path> rink_model_path();
bool feature_matcher_model_override_configured(ControlPointMatcher matcher);
absl::StatusOr<std::filesystem::path> feature_matcher_model_path(ControlPointMatcher matcher);
absl::StatusOr<std::string> feature_matcher_asset_to_ensure(ControlPointMatcher matcher);

} // namespace hm::stitching
