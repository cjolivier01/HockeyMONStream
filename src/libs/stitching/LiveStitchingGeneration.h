#pragma once

#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::stitching {

struct LiveStitchedOutputAuthorization {
  std::string pending_generation;
  std::string authorization_id;
  bool invalidate_scoreboard{false};
  std::string scoreboard_property_value;

  explicit operator bool() const {
    return !pending_generation.empty() && !authorization_id.empty();
  }
};

// Retires an authorization whose owning process is no longer active. The
// completed generation's saved scoreboard geometry is restored before the
// pending chain is removed. Returns true when config was reconciled.
absl::StatusOr<bool> reconcile_inactive_live_stitched_output_authorization(const std::string& game_dir);

// Authorizes one exact live rotation generation. The authorization preserves
// the completed generation's Hugin identity and dimensions and records the
// pending generation without invalidating completed-generation geometry. A
// later call supersedes an earlier authorization.
absl::StatusOr<LiveStitchedOutputAuthorization> authorize_live_stitched_output_rotation(
    const std::string& game_dir,
    double post_stitch_rotate_degrees,
    const std::string& authorization_id);

// Rolls back an abandoned authorization only when both its generation and
// unique epoch still own the pending slot. Returns the predecessor restored by
// the rollback, if any.
absl::StatusOr<std::optional<LiveStitchedOutputAuthorization>> rollback_live_stitched_output_rotation(
    const std::string& game_dir,
    const std::string& pending_generation,
    const std::string& authorization_id);

// Invalidates geometry after the backend accepts a previously authorized live
// rotation. Publication of the exact generation is also accepted as an
// idempotent completion because that transaction performs the same invalidation.
absl::Status commit_live_stitched_output_rotation(
    const std::string& game_dir,
    const std::string& pending_generation,
    const std::string& authorization_id);

} // namespace hm::stitching
