#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "yaml-cpp/yaml.h"

namespace hm::stitching {

// Serializes every read-modify-write operation on a game's config.yaml.
class GameConfigLock final {
 public:
  ~GameConfigLock();

  GameConfigLock(const GameConfigLock&) = delete;
  GameConfigLock& operator=(const GameConfigLock&) = delete;

  static absl::StatusOr<std::unique_ptr<GameConfigLock>> Acquire(const std::filesystem::path& game_dir);

 private:
  explicit GameConfigLock(int descriptor) : descriptor_(descriptor) {}

  int descriptor_{-1};
};

// Acquires the config lock followed by the rink-publication lock and recovers
// any interrupted rink transaction before a config reader observes YAML.
class GameConfigTransactionLock final {
 public:
  ~GameConfigTransactionLock();

  GameConfigTransactionLock(const GameConfigTransactionLock&) = delete;
  GameConfigTransactionLock& operator=(const GameConfigTransactionLock&) = delete;

  static absl::StatusOr<std::unique_ptr<GameConfigTransactionLock>> Acquire(const std::filesystem::path& game_dir);

 private:
  struct State;
  explicit GameConfigTransactionLock(std::unique_ptr<State> state);

  std::unique_ptr<State> state_;
};

// Durably replaces config.yaml. The caller must hold GameConfigLock while
// constructing contents from the previous config and until this returns.
absl::Status publish_game_config(const std::filesystem::path& game_dir, const std::string& contents);

// Durably publishes config.yaml while removing every rink_mask_*.png as one
// recoverable generation. The caller must hold GameConfigTransactionLock.
// Returns the number of masks removed.
absl::StatusOr<size_t> publish_game_config_without_rink_masks(
    const std::filesystem::path& game_dir,
    const std::string& contents);

// Loads one config generation while holding the config/rink transaction lock.
// A missing file is represented by an empty optional node.
absl::StatusOr<std::optional<YAML::Node>> load_game_config_file(const std::filesystem::path& config_path);

// Applies only changes made between baseline and desired to latest. This is a
// three-way merge for independently owned config paths, not a conflict
// resolver: when both owners change the same path, desired wins.
YAML::Node apply_game_config_diff(const YAML::Node& baseline, const YAML::Node& desired, const YAML::Node& latest);

// Reverses baseline back to desired without overwriting a newer update to the
// same path. Sequence entries restored by the rollback are merged with newer
// entries instead of replacing them.
YAML::Node merge_game_config_rollback(const YAML::Node& baseline, const YAML::Node& desired, const YAML::Node& latest);

} // namespace hm::stitching
