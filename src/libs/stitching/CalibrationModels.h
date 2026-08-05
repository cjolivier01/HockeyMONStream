#pragma once

#include <filesystem>

#include "absl/status/statusor.h"

namespace hm::stitching {

absl::StatusOr<std::filesystem::path> rink_model_path();
absl::StatusOr<std::filesystem::path> feature_matcher_model_path();

} // namespace hm::stitching
