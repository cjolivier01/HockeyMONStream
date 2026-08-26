#pragma once

#include <filesystem>
#include <string>

#include "absl/status/status.h"

namespace hm::stitching {

// Publishes a complete journal state through an fsynced temporary file and an
// atomic rename, then makes the renamed directory entry durable.
absl::Status publish_transaction_state(const std::filesystem::path& transaction, const std::string& contents);

} // namespace hm::stitching
