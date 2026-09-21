#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

struct StitchingExperimentSettings {
  int control_points{0};
  int frame_count{0};
  std::string stitch_frame_time;
  std::optional<std::array<double, 3>> rink_rotation_degrees;
};

struct StitchingExperimentWorkspace {
  std::filesystem::path root;
  std::filesystem::path game_directory;
  std::string game_id;
  std::string invalidation_id;
  StitchingExperimentSettings settings;
};

// Creates an isolated game whose media entries are read-only symlinks to the
// selected game. Calibration output is therefore private to one candidate.
absl::StatusOr<StitchingExperimentWorkspace> CreateStitchingExperimentWorkspace(
    const std::filesystem::path& source_game_directory,
    const std::filesystem::path& experiment_root,
    const StitchingExperimentSettings& settings,
    int sequence);

// After the runner and its helpers stop, validates the generation and durably
// marks the private calibration complete before offering preview or promotion.
absl::Status CompleteStitchingExperimentWorkspace(const StitchingExperimentWorkspace& experiment);

// Builds the exact config document published by selection. Exposed so the
// geometry invalidation contract can be checked without generating images.
absl::StatusOr<std::string> BuildStitchingExperimentSelectionConfig(
    const std::filesystem::path& experiment_config,
    const std::filesystem::path& game_config);

// Publishes the already-generated candidate maps/seam without recalibrating,
// then updates only stitching-owned config and invalidates dependent rink data.
absl::Status PromoteStitchingExperiment(
    const StitchingExperimentWorkspace& experiment,
    const std::filesystem::path& game_directory);
