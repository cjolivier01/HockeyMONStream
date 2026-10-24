#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "yaml-cpp/yaml.h"

namespace hm::program {
struct ProgramCalibrationRequest {
  std::optional<std::string> reference_time, invalidation_id;
};

// Compare against saved configuration, independently of launch overrides.
absl::Status ValidateProgramCalibrationSelection(const YAML::Node& saved, const ProgramCalibrationRequest& request);

// Re-read config under the config/rink lock while the caller still holds the
// Hugin artifact lock for its load snapshot. Publication cannot replace the
// loaded artifact generation between this check and payload loading.
absl::Status ValidateLockedProgramCalibration(
    const std::filesystem::path& game,
    const ProgramCalibrationRequest& request,
    const stitching::HuginProject::ArtifactLock& artifact_lock);
} // namespace hm::program
