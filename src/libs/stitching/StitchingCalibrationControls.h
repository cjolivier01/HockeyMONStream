#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "hstream/src/libs/stitching/GameConfig.h"

namespace hm::stitching {

// Callers supply resolved controls. This layer never reads account defaults or
// probes recordings; the GUI and authenticated worker own those snapshots.
struct StitchingCalibrationControls {
  int control_points{0}, frame_count{0}, max_output_width{0};
  std::string frame_time, sync_method, matcher, resolution, execution_provider, backend, projection;
  ControlPointFiltering filtering;
  StitchCameraSelection camera;
  bool autooptimizer{false};
  std::vector<double> projection_parameters;
  StitchProjectionFraming framing;
};

struct StitchingCalibrationState {
  StitchingCalibrationControls controls;
  bool control_points_present{false}, frame_count_present{false}, frame_time_valid{true};
  std::optional<int> selected_frame_count;
  std::string status, stale_from;
  bool artifacts_invalidated{false};
};

struct StitchingCalibrationChanges {
  bool control_points{false}, frame_count{false}, frame_time{false}, sync_method{false}, matcher{false};
  bool backend{false}, camera{false}, autooptimizer{false}, projection{false}, parameters{false}, framing{false};
  bool width{false}, canvas_required{false}, canvas_cleanup{false}, force{false}, sources{false};
  bool geometry() const;
};

enum class StitchingCalibrationCleanup { kNone, kAll, kControlPoints };
const char* StitchingCalibrationCleanupName(StitchingCalibrationCleanup cleanup);

struct StitchingCalibrationPlan {
  StitchingCalibrationChanges changes;
  bool required{false}, alignment_reused{false}, retain_selected_frames{false}, retain_manual_matches{false};
  std::string start_stage, owner;
  StitchingCalibrationCleanup cleanup{StitchingCalibrationCleanup::kNone};
};

absl::StatusOr<StitchingCalibrationControls> ReadStitchingCalibrationControls(const YAML::Node& effective);
// Resolve each layer before merging so legacy native aliases in a private game
// retain precedence over a canonical baseline value. Explicit null means Auto.
absl::StatusOr<int> ResolveStitchingCalibrationMaxWidth(const YAML::Node& private_config, const YAML::Node& defaults);
absl::StatusOr<StitchingCalibrationState> ReadStitchingCalibrationState(
    const YAML::Node& private_config,
    const YAML::Node& effective);

// Apply the shared sampling/input transition without disk I/O or a solve claim.
// Returns whether the existing selected frames survive. Rejected time edits do
// not mutate the document. Clean and Play use the same native state precedence.
absl::StatusOr<bool> ApplyStitchingCalibrationInputEdits(
    const StitchingCalibrationState& before,
    const StitchingCalibrationControls& desired,
    bool force,
    bool sources_changed,
    YAML::Node& config);

// Exact manual Clean Stitching config policy shared by Qt and the web adapter.
// Call after successful artifact cleanup under the game config transaction lock.
// Preserve retained frame selections, operator controls and unrelated settings.
int RemoveManualStitchingCleanConfigKeys(YAML::Node& config);

// Camera-role edits invalidate oriented inputs and synchronization. Only active
// config references are removed; immutable historical bundles stay untouched.
bool InvalidateStitchingSourceInputs(YAML::Node& config);

// Requires artifact -> config locks. Desired private controls have already been
// written by the caller. On failure both that document and disk are unchanged.
// The shared planner owns selected-input retention, manual matches, reframe
// capture, dependency stage, cleanup mode and the new generation claim.
absl::StatusOr<StitchingCalibrationPlan> PrepareStitchingCalibrationControlsLocked(
    const std::filesystem::path& game,
    const StitchingCalibrationState& before,
    const StitchingCalibrationControls& desired,
    const YAML::Node& before_effective,
    const YAML::Node& desired_effective,
    const std::string& owner,
    bool force,
    bool sources_changed,
    bool canvas_required,
    bool canvas_cleanup,
    YAML::Node& desired_private);

// The cleanup implementations are shared with the CLI. This marks completion
// only while the pending owner/state and controls still match the prepared
// document, apart from the cache removals owned by native cleanup.
// The caller must invoke it after native cleanup, never merely after launch.
absl::Status CompleteStitchingCalibrationCleanup(
    const std::filesystem::path& game,
    const StitchingCalibrationPlan& plan,
    const YAML::Node& prepared_config);

} // namespace hm::stitching
