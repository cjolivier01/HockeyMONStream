#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "yaml-cpp/yaml.h"

#include "hstream/src/libs/stitching/StitchingAlgorithms.h"

namespace hm::stitching {

struct StitchProjectionFraming {
  bool auto_fov{false};
  double horizontal_fov{180.0};
  bool auto_canvas{true};
  bool auto_crop{false};

  bool operator==(const StitchProjectionFraming& other) const {
    return auto_fov == other.auto_fov && horizontal_fov == other.horizontal_fov &&
        auto_canvas == other.auto_canvas && auto_crop == other.auto_crop;
  }
  bool operator!=(const StitchProjectionFraming& other) const { return !(*this == other); }
};

struct StitchingBackendChoices {
  std::string control_point_matcher;
  std::string mapping_backend;
  std::string projection;
  bool run_autooptimizer{false};
  std::vector<double> projection_parameters;
  StitchProjectionFraming projection_framing;
};

// Projection parameters are stored under
// stitching.projection_parameters.<canonical-projection-name>. Missing entries
// use libpano/Hugin defaults. The full map is validated on every read so a
// malformed inactive projection cannot become active later without detection.
absl::StatusOr<std::vector<double>> read_stitch_projection_parameters(
    const YAML::Node& config,
    StitchProjection projection);
void write_stitch_projection_parameters(
    YAML::Node& config,
    StitchProjection projection,
    const std::vector<double>& parameters);

// Controls how pano_modify frames the selected projection. AUTO options ask
// Hugin to recompute that property; disabled options preserve the optimizer's
// corresponding PTO setting. horizontal_fov is used only when auto_fov=false.
absl::StatusOr<StitchProjectionFraming> read_stitch_projection_framing(const YAML::Node& config);
void write_stitch_projection_framing(YAML::Node& config, const StitchProjectionFraming& framing);
absl::Status ValidateStitchProjectionFraming(
    StitchProjection projection,
    const std::vector<double>& projection_parameters,
    const StitchProjectionFraming& framing);

// Serializes every read-modify-write operation on a game's config.yaml.
class GameConfigLock final {
 public:
  ~GameConfigLock();

  GameConfigLock(const GameConfigLock&) = delete;
  GameConfigLock& operator=(const GameConfigLock&) = delete;

  static absl::StatusOr<std::unique_ptr<GameConfigLock>> Acquire(const std::filesystem::path& game_dir);

  // Returns Unavailable instead of waiting when another config transaction is active.
  static absl::StatusOr<std::unique_ptr<GameConfigLock>> TryAcquire(const std::filesystem::path& game_dir);

 private:
  explicit GameConfigLock(int descriptor) : descriptor_(descriptor) {}

  int descriptor_{-1};
};

// Acquires the config lock followed by the rink-publication lock and recovers
// any interrupted rink transaction before a config reader observes YAML.
class GameConfigTransactionLock final {
 public:
  ~GameConfigTransactionLock();

  GameConfigTransactionLock(const GameConfigTransactionLock&) = delete;
  GameConfigTransactionLock& operator=(const GameConfigTransactionLock&) = delete;

  static absl::StatusOr<std::unique_ptr<GameConfigTransactionLock>> Acquire(const std::filesystem::path& game_dir);

  // Returns Unavailable instead of waiting when either publication lock is
  // active. Interrupted rink transactions are recovered before success.
  static absl::StatusOr<std::unique_ptr<GameConfigTransactionLock>> TryAcquire(const std::filesystem::path& game_dir);

 private:
  struct State;
  explicit GameConfigTransactionLock(std::unique_ptr<State> state);

  std::unique_ptr<State> state_;
};

// Durably replaces config.yaml. The caller must hold GameConfigLock while
// constructing contents from the previous config and until this returns.
absl::Status publish_game_config(const std::filesystem::path& game_dir, const std::string& contents);

// Durably replaces one named file without exposing a truncated/partial
// generation to concurrent readers.
absl::Status publish_named_file(const std::filesystem::path& path, const std::string& contents);

// Durably publishes config.yaml while removing every rink_mask_*.png and,
// when requested, the stitched calibration snapshot as one recoverable
// generation. The caller must hold GameConfigTransactionLock. Returns the
// number of artifacts removed.
absl::StatusOr<size_t> publish_game_config_without_rink_masks(
    const std::filesystem::path& game_dir,
    const std::string& contents,
    bool remove_stitched_snapshot = false);

// Loads one config generation while holding the config/rink transaction lock.
// A missing file is represented by an empty optional node.
absl::StatusOr<std::optional<YAML::Node>> load_game_config_file(const std::filesystem::path& config_path);

// Requires the supplied document to contain the expected pending stitching
// invalidation. Callers use this while holding GameConfigTransactionLock so
// validation and a dependent mutation share one transaction.
absl::Status validate_pending_stitching_invalidation(
    const YAML::Node& config,
    const std::string& expected_invalidation_id);

// Accepts the same generation while it is pending or after it has completed.
// Long-lived runtime artifact owners use this to regenerate downstream data
// until a newer invalidation replaces their generation ID.
absl::Status validate_stitching_generation_owner(const YAML::Node& config, const std::string& expected_invalidation_id);

// Loads and validates config_path without acquiring another lock. The caller
// must hold GameConfigTransactionLock so validation and its dependent artifact
// publication cannot be separated by a newer invalidation.
absl::Status validate_pending_stitching_invalidation_file_locked(
    const std::filesystem::path& config_path,
    const std::string& expected_invalidation_id);

absl::Status validate_stitching_generation_owner_file_locked(
    const std::filesystem::path& config_path,
    const std::string& expected_invalidation_id);

// Rejects Hugin publication while a live stitched-output generation owns the
// current artifacts. The caller must hold GameConfigTransactionLock.
absl::Status validate_no_pending_live_stitched_output_authorization_file_locked(
    const std::filesystem::path& config_path);

// Process-start and Linux boot identities prevent a crashed live controller
// from retaining publication authority and fence PID reuse across reboots.
absl::StatusOr<std::string> current_live_stitched_output_owner_process();
absl::StatusOr<bool> live_stitched_output_owner_process_is_active(std::string_view identity);
absl::StatusOr<bool> live_stitched_output_authorization_is_active(const YAML::Node& config);

// Claims one immutable algorithm tuple for a pending/completed calibration
// generation. A concurrent process may share the generation only when it uses
// the same effective choices.
absl::Status reserve_stitching_backend_generation(
    const std::filesystem::path& game_dir,
    const std::string& expected_invalidation_id,
    const StitchingBackendChoices& expected_choices);

// Adds or validates the immutable backend claim in a document that already
// contains the matching worker-visible tuple. The caller must hold
// GameConfigTransactionLock and publish this document before releasing it so
// the claim and tuple can never become separate config generations.
absl::Status reserve_stitching_backend_generation_in_config(
    YAML::Node& config,
    const std::string& expected_invalidation_id,
    const StitchingBackendChoices& expected_choices);

// Validates both the immutable generation claim and the worker-visible
// stitching tuple. The file variant requires GameConfigTransactionLock.
absl::Status validate_stitching_backend_generation(
    const YAML::Node& config,
    const std::string& expected_invalidation_id,
    const StitchingBackendChoices& expected_choices);
absl::Status validate_stitching_backend_generation_file_locked(
    const std::filesystem::path& config_path,
    const std::string& expected_invalidation_id,
    const StitchingBackendChoices& expected_choices);

// Applies only changes made between baseline and desired to latest. This is a
// three-way merge for independently owned config paths, not a conflict
// resolver: when both owners change the same path, desired wins.
YAML::Node apply_game_config_diff(const YAML::Node& baseline, const YAML::Node& desired, const YAML::Node& latest);

// Reverses baseline back to desired without overwriting a newer update to the
// same path. Sequence entries restored by the rollback are merged with newer
// entries instead of replacing them.
YAML::Node merge_game_config_rollback(const YAML::Node& baseline, const YAML::Node& desired, const YAML::Node& latest);

} // namespace hm::stitching
