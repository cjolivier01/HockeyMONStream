#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

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

} // namespace hm::stitching
