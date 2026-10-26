#pragma once

#include <filesystem>

#include "hstream/src/libs/player_analytics/ModelContract.h"

namespace hm::player_analytics {

// CPU-only checks shared by Qt's Custom chooser and private web imports. SDK
// and GPU compatibility remain the inference runtime's responsibility.
absl::StatusOr<ModelManifest> ValidatePreparedModelBundle(
    const std::filesystem::path& directory,
    ModelFeature feature,
    std::size_t minimum_batch);
absl::StatusOr<std::filesystem::path> ValidatePreparedReidConfig(const std::filesystem::path& config);

} // namespace hm::player_analytics
