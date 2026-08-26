#pragma once

#include <string>

#include "absl/status/statusor.h"

namespace hm::stitching {

// Authorizes one exact live rotation generation to replace completed rink
// geometry. The authorization preserves the completed generation's Hugin
// identity and dimensions; a later call supersedes an earlier authorization.
// Returns true when the authorized generation differs from the completed one.
absl::StatusOr<bool> authorize_live_stitched_output_rotation(
    const std::string& game_dir,
    double post_stitch_rotate_degrees);

} // namespace hm::stitching
