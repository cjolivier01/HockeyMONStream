#pragma once

#include <filesystem>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::game_storage {

// A game always means the recording/publication root, never its private state
// directory. These path helpers do no I/O and never read the former flat layout.
std::filesystem::path StateDirectory(const std::filesystem::path& game);
std::filesystem::path StatePath(const std::filesystem::path& game, const std::filesystem::path& relative);
absl::Status EnsureStateDirectory(const std::filesystem::path& game);

// Explicit work-root settings may resolve inside a game (including through a
// directory alias). Keep that work private too; external work remains external.
absl::StatusOr<std::filesystem::path> ConfineWorkDirectory(
    const std::filesystem::path& game,
    const std::filesystem::path& requested);

// Inventory is descriptive, not deletion authority. Existing transaction
// journals and publication receipts remain authoritative. Refresh at operation
// boundaries, never on the playback/frame path. Symlink targets are not visited.
absl::Status RefreshManifest(const std::filesystem::path& game, const std::string& producer);
absl::Status RegisterPublished(
    const std::filesystem::path& game,
    const std::filesystem::path& path,
    const std::string& producer,
    const std::string& run_id = {});

// Explicitly record imported recordings/aliases without claiming ownership of
// their content or following linked targets. Ordinary preexisting recordings
// are not automatically adopted into this list.
absl::Status RegisterInput(
    const std::filesystem::path& game,
    const std::filesystem::path& path,
    const std::string& producer);

} // namespace hm::game_storage
