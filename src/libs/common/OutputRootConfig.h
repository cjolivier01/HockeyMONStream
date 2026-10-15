#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

namespace hm::user_config {

// Resolve admission storage before launching the CLI, using its baseline,
// user, game and --options layers. Pipeline documents are underlaid beneath
// pipeline and do not define this top-level path.
absl::StatusOr<std::filesystem::path> output_root_for_game(
    const std::filesystem::path& game,
    const std::vector<std::string>& option_groups = {});

} // namespace hm::user_config
