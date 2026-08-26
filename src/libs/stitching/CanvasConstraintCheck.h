#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::stitching {

class CanvasConstraintArtifactLock {
 public:
  ~CanvasConstraintArtifactLock();
  CanvasConstraintArtifactLock(const CanvasConstraintArtifactLock&) = delete;
  CanvasConstraintArtifactLock& operator=(const CanvasConstraintArtifactLock&) = delete;

 private:
  friend absl::StatusOr<struct LightweightCanvasConstraintCheck> try_lock_canvas_constraint_check(
      const std::filesystem::path& game_dir,
      size_t max_output_width);
  friend absl::StatusOr<std::unique_ptr<CanvasConstraintArtifactLock>> try_lock_canvas_constraint_artifacts(
      const std::filesystem::path& game_dir);
  friend absl::StatusOr<std::unique_ptr<CanvasConstraintArtifactLock>> lock_canvas_constraint_artifacts(
      const std::filesystem::path& game_dir);
  explicit CanvasConstraintArtifactLock(int descriptor) : descriptor_(descriptor) {}
  int descriptor_{-1};
};

struct LightweightCanvasConstraintCheck {
  std::unique_ptr<CanvasConstraintArtifactLock> artifact_lock;
  bool artifacts_compatible{false};
  bool requires_regeneration{false};
};

struct CanvasConstraintCompatibility {
  bool artifacts_compatible{false};
  bool requires_regeneration{false};
};

inline constexpr char kStitchCanvasProvenanceArtifact[] = "stitching_canvas_provenance";
inline constexpr char kStitchGenerationArtifact[] = "stitching_generation_id";

// Returns the effective platform/runtime canvas limit. Malformed environment
// overrides are ignored consistently by generation and validation paths.
std::optional<size_t> live_stitch_max_canvas_dimension();

// Shared by the lightweight checker and Hugin publication so crash recovery
// and the accepted transaction manifests cannot diverge.
const std::vector<std::string>& required_stitch_artifact_names();
const std::vector<std::string>& stitch_artifact_names();
absl::Status recover_stitch_transactions_locked(const std::filesystem::path& game_dir);
absl::Status fsync_stitch_path(const std::filesystem::path& path, bool directory = false);
absl::Status write_stitch_transaction_file(const std::filesystem::path& path, const std::string& contents);
absl::Status prepare_stitch_generation_publication(
    const std::filesystem::path& staging,
    const std::filesystem::path& game_dir);
absl::Status rebind_stitch_generation_artifact(
    const std::filesystem::path& transaction,
    const std::filesystem::path& game_dir);

// Reviews one stable artifact generation. The caller must hold the Hugin
// artifact lock for the complete call.
absl::StatusOr<CanvasConstraintCompatibility> check_canvas_constraint_locked(
    const std::filesystem::path& game_dir,
    size_t max_output_width);

// Performs the same provenance and artifact-layout checks without decoding
// the seam payload. Intended for interactive preflight only; pipeline startup
// must use check_canvas_constraint_locked for authoritative validation.
absl::StatusOr<CanvasConstraintCompatibility> check_canvas_constraint_metadata_locked(
    const std::filesystem::path& game_dir,
    size_t max_output_width);

// Attempts the artifact lock and performs transaction recovery without
// inspecting mapping payloads. This lets callers defer TIFF/PNG I/O until they
// have confirmed that the effective width changed.
absl::StatusOr<std::unique_ptr<CanvasConstraintArtifactLock>> try_lock_canvas_constraint_artifacts(
    const std::filesystem::path& game_dir);

// Acquires the same artifact lock while waiting for an active Hugin producer.
// Live config transactions use this before taking GameConfigTransactionLock.
absl::StatusOr<std::unique_ptr<CanvasConstraintArtifactLock>> lock_canvas_constraint_artifacts(
    const std::filesystem::path& game_dir);

// Returns the exact identity embedded in stitched-output generations. The
// caller must hold the stitching artifact lock for the complete call.
absl::StatusOr<std::string> stitch_artifact_generation_id_locked(const std::filesystem::path& game_dir);

// Attempts the artifact lock without waiting. A missing lock means another
// generation owns the artifacts, so callers must fail closed without blocking
// an interactive UI.
absl::StatusOr<LightweightCanvasConstraintCheck> try_lock_canvas_constraint_check(
    const std::filesystem::path& game_dir,
    size_t max_output_width);

} // namespace hm::stitching
