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
  // An explicit size is frozen for this solve; legacy nullopt retains config inheritance.
  std::optional<std::string> control_point_resolution;
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

// Returns the saved immutable selection for this count, or an empty string when
// a different count explicitly requests new frames. Same-count anchor conflicts
// and malformed plans are errors; this query does not require source media.
absl::StatusOr<std::string> ReusableStitchingExperimentSelectionFingerprint(
    const std::filesystem::path& game_directory,
    const StitchingExperimentSettings& settings);

// Describes the current main calibration for read-only inspection/preview.
// An uncalibrated game with no frozen plan returns no row.
absl::StatusOr<std::optional<StitchingExperimentWorkspace>> MainStitchingExperimentWorkspace(
    const std::filesystem::path& game_directory,
    const StitchingExperimentSettings& displayed_settings);

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

// Reuses an already frozen selection, including one whose subsequent solve
// failed. Validates its source bindings and the pending destination without
// requiring the original scoring geometry to match this candidate's solve.
absl::Status ReuseStitchingExperimentPlayerSelection(
    const StitchingExperimentWorkspace& source,
    const StitchingExperimentWorkspace& candidate);

struct StitchingExperimentSelectedFrame {
  uint64_t timeline_ns{0};
  std::array<std::string, 2> camera_paths;
  std::array<uint64_t, 2> source_pts_ns{};
  std::array<double, 2> source_seconds{};
  std::array<uint64_t, 2> decoded_sequences{};
  size_t eligible_people{0};
  std::array<size_t, 3> size_band_counts{};
  double quality{0};
  std::vector<uint16_t> coverage;
  std::array<std::filesystem::path, 2> thumbnails;
  // Generation-owned combined stills: matched endpoints, then endpoints + lines.
  // Missing files are expected for legacy rows or pairs that failed matching.
  std::array<std::filesystem::path, 2> match_images;
};

struct StitchingExperimentFrameInspection {
  bool player_selected{false};
  size_t expected_pair_count{0};
  uint64_t anchor_ns{0};
  std::string fingerprint;
  std::string source_validation;
  std::vector<StitchingExperimentSelectedFrame> frames;
};

// Reads the highlighted candidate's captured ordinary frames or frozen player
// selection, including partial captures from a failed solve. Thumbnail paths
// are confined to its private bounded inspection set.
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
