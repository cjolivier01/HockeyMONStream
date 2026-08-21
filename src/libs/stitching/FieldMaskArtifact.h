#pragma once

#include "absl/status/status.h"

#include <functional>
#include <string>

namespace hm::stitching {

// Runs a bounded consumer against the current rink_mask_0.png while holding
// the Hugin and config/rink transaction locks. Generation ownership is
// revalidated before the consumer runs, so a preview cannot cache bytes from
// an artifact being replaced by another calibration generation.
absl::Status visit_current_field_mask(
    const std::string& game_dir,
    const std::string& expected_output_generation,
    const std::string& expected_invalidation_id,
    const std::function<absl::Status(const std::string& mask_path)>& consumer);

} // namespace hm::stitching
