#pragma once

#include <filesystem>
#include <map>
#include <string>

#include "absl/status/status.h"

namespace hm::stitching::orientation_internal {

using VideoChapterMap = std::map<int, std::string>;

// Persists an already-resolved camera ordering. Runtime Program launches may
// legitimately add this derived mapping while retaining a completed stitching
// generation owner; a different generation ID must still fence the write.
absl::Status save_orientation_config(
    const std::filesystem::path& game_dir,
    const VideoChapterMap& left,
    const VideoChapterMap& right,
    const std::string& expected_invalidation_id);

} // namespace hm::stitching::orientation_internal
