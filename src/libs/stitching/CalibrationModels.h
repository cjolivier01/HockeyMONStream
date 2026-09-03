#pragma once

#include <filesystem>
#include <string>

#include "absl/status/statusor.h"
#include "hstream/src/libs/stitching/ControlPointMatcher.h"

namespace hm::stitching {

absl::StatusOr<std::filesystem::path> rink_model_path();
bool feature_matcher_model_override_configured(ControlPointMatcher matcher);
// Resolves the runtime target without requiring it to exist yet. This lets the
// asset manager download a stock cache target or verify a packaged target.
absl::StatusOr<std::filesystem::path> feature_matcher_model_target_path(ControlPointMatcher matcher);
absl::StatusOr<std::filesystem::path> feature_matcher_model_path(ControlPointMatcher matcher);
absl::StatusOr<std::string> feature_matcher_asset_to_ensure(ControlPointMatcher matcher);
// Pins subsequent matcher construction to the exact path already verified by
// the asset manager.
absl::Status bind_feature_matcher_model_path(ControlPointMatcher matcher, const std::filesystem::path& verified_path);

} // namespace hm::stitching
