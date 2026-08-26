#pragma once

#include <filesystem>
#include <string>

#include "absl/status/status.h"

namespace hm::stitching {

// Snapshots one opened regular file into a rollback directory without following
// symlinks or trusting that the source pathname remains bound to the same inode.
absl::Status snapshot_regular_file_for_rollback(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    bool force_portable_fallback = false);

// Publishes a complete journal state through an fsynced temporary file and an
// atomic rename, then makes the renamed directory entry durable.
absl::Status publish_transaction_state(const std::filesystem::path& transaction, const std::string& contents);

} // namespace hm::stitching
