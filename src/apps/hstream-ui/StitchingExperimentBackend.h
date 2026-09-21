#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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
absl::Status CompleteStitchingExperimentWorkspace(
    const StitchingExperimentWorkspace& experiment,
    bool require_ice_mask = false);

struct StitchingExperimentPlayerSelection {
  bool available{false};
  std::string diagnostic;
};

// Validates a completed scan against its baseline and current media, then saves
// the immutable selection in the still-pending candidate. Unavailable coverage
// is a successful quality result; malformed/stale reports remain errors.
absl::StatusOr<StitchingExperimentPlayerSelection> PreparePlayerSelectedStitchingExperiment(
    const StitchingExperimentWorkspace& baseline,
    const StitchingExperimentWorkspace& candidate,
    const std::filesystem::path& report_path,
    int search_duration_seconds = 60);

struct StitchingExperimentSelectedFrame {
  uint64_t timeline_ns{0};
  std::array<std::string, 2> camera_paths;
  std::array<uint64_t, 2> source_pts_ns{};
  std::array<uint64_t, 2> decoded_sequences{};
  size_t eligible_people{0};
  std::array<size_t, 3> size_band_counts{};
  double quality{0};
  std::vector<uint16_t> coverage;
  std::array<std::filesystem::path, 2> thumbnails;
};

struct StitchingExperimentFrameInspection {
  uint64_t anchor_ns{0};
  std::string fingerprint;
  std::string source_validation;
  std::vector<StitchingExperimentSelectedFrame> frames;
};

// Reads frozen identities/diagnostics even if the subsequent solve failed.
// Thumbnail paths are confined to this plan's private bounded inspection set.
absl::StatusOr<StitchingExperimentFrameInspection> InspectStitchingExperimentFrames(
    const StitchingExperimentWorkspace& experiment);

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
