#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>

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

// Reviews one stable artifact generation. The caller must hold the Hugin
// artifact lock for the complete call.
absl::StatusOr<CanvasConstraintCompatibility> check_canvas_constraint_locked(
    const std::filesystem::path& game_dir,
    size_t max_output_width);

// Attempts the artifact lock without waiting. A missing lock means another
// generation owns the artifacts, so callers must fail closed without blocking
// an interactive UI.
absl::StatusOr<LightweightCanvasConstraintCheck> try_lock_canvas_constraint_check(
    const std::filesystem::path& game_dir,
    size_t max_output_width);

} // namespace hm::stitching
