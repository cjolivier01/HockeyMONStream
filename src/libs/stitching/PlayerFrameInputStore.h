#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <vector>

#include "hstream/src/libs/stitching/PlayerFrameSelection.h"

namespace hm::stitching {

enum class PlayerFrameInputValidation { kFull, kInspection };

struct PlayerFrameInputSet {
  std::vector<std::array<std::filesystem::path, 2>> images;
  std::vector<std::array<std::filesystem::path, 2>> thumbnails;
  // Retain validated identities so a copy can verify its staged bytes against
  // what was validated, even if a source file changes during publication.
  std::vector<std::array<std::string, 2>> image_digests;
  std::vector<std::array<std::string, 2>> thumbnail_digests;
};

// Immutable, complete input bundles live alongside the game's calibration.
// Absence alone returns nullopt; any present but invalid bundle is an error.
// Inspection validates the manifest and JPEGs without hashing full PNG payloads.
// These helpers never acquire artifact/config locks. Publication acquires only
// its own store lock, after any caller-held artifact/config locks.
absl::StatusOr<std::optional<PlayerFrameInputSet>> LoadPlayerFrameInputs(
    const std::filesystem::path& game_directory,
    const PlayerFrameSelectionPlan& plan,
    PlayerFrameInputValidation validation = PlayerFrameInputValidation::kFull);
absl::Status PublishPlayerFrameInputs(
    const std::filesystem::path& game_directory,
    const PlayerFrameSelectionPlan& plan,
    const std::vector<std::array<std::filesystem::path, 2>>& images);
absl::Status CopyPlayerFrameInputs(
    const std::filesystem::path& source_game_directory,
    const std::filesystem::path& destination_game_directory,
    const PlayerFrameSelectionPlan& plan);

} // namespace hm::stitching
