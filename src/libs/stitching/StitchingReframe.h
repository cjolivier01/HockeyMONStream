#pragma once

#include <array>
#include <filesystem>
#include <string>

#include "absl/status/statusor.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "yaml-cpp/yaml.h"

namespace hm::stitching {

struct StitchingReframeIntent {
  std::string source_generation;
  std::string desired_owner;
  std::array<std::string, 2> source_image_sha256;
  HuginProject::CanvasProvenance source_provenance;
  StitchingBackendChoices desired_choices;
  size_t desired_max_output_width{0};
  size_t desired_max_output_dimension{0};
};

// Presence includes malformed non-null records, which must fail validation
// before any caller attempts ordinary cleanup or matching.
bool HasStitchingReframeIntent(const YAML::Node& config);
void ClearStitchingReframeIntent(YAML::Node& config);

// All Locked operations require artifact -> config locks held by the caller.
// Inputs are effective configurations. Missing values inherit the shared
// baseline and user config; explicit game/runtime values always take priority.
// Capture a completed alignment before editing it, or retain an existing
// intent's original source while replacing its desired view and owner.
// false means no reframe is needed, an explicit solve input changed, or no
// completed source has ever existed. A requested geometry change with an
// unsupported/unverifiable completed source fails instead of rerunning a solve.
// This only mutates desired_private; the caller publishes it atomically with
// the matching pending owner. Earlier input/features invalidations stay ordinary.
absl::StatusOr<bool> PrepareStitchingReframeIntentLocked(
    const std::filesystem::path& game_directory,
    const YAML::Node& before_effective,
    const YAML::Node& desired_effective,
    const std::string& desired_owner,
    YAML::Node& desired_private);

// Revalidate source artifacts/stills, retained selected inputs, current owner,
// and the complete solve/view snapshots. Does not decode original recordings
// or acquire another lock. Failure never authorizes an ordinary full solve.
absl::StatusOr<StitchingReframeIntent> ValidateStitchingReframeIntentLocked(
    const std::filesystem::path& game_directory,
    const YAML::Node& effective_config);

} // namespace hm::stitching
