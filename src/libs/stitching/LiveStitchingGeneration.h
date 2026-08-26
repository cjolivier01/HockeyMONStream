#pragma once

#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::stitching {

// Authorizes one exact live rotation generation. The authorization preserves
// the completed generation's Hugin identity and dimensions and records the
// pending generation without invalidating completed-generation geometry. A
// later call supersedes an earlier authorization.
// Returns true when the authorized generation differs from the completed one.
absl::StatusOr<bool> authorize_live_stitched_output_rotation(
    const std::string& game_dir,
    double post_stitch_rotate_degrees);

// Invalidates geometry for a previously authorized live rotation. The pending
// generation must still exactly match the requested rotation.
absl::Status commit_live_stitched_output_rotation(const std::string& game_dir, double post_stitch_rotate_degrees);

} // namespace hm::stitching
