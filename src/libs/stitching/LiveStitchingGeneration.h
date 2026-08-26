#pragma once

#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::stitching {

struct LiveStitchedOutputAuthorization {
  // Empty when the requested rotation already matches the completed output or
  // when no completed output exists to fence.
  std::string pending_generation;
};

// Authorizes one exact live rotation generation. The authorization preserves
// the completed generation's Hugin identity and dimensions and records the
// pending generation without invalidating completed-generation geometry. A
// later call supersedes an earlier authorization.
absl::StatusOr<LiveStitchedOutputAuthorization> authorize_live_stitched_output_rotation(
    const std::string& game_dir,
    double post_stitch_rotate_degrees);

// Removes an abandoned authorization only when the exact pending generation
// still owns it. Returns false when it was already consumed or superseded.
absl::StatusOr<bool> cancel_live_stitched_output_rotation(
    const std::string& game_dir,
    const std::string& pending_generation);

// Invalidates geometry after the backend accepts a previously authorized live
// rotation. Publication of the exact generation is also accepted as an
// idempotent completion because that transaction performs the same invalidation.
absl::Status commit_live_stitched_output_rotation(const std::string& game_dir, const std::string& pending_generation);

} // namespace hm::stitching
