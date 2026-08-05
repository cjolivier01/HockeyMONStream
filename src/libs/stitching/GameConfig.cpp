#include "hstream/src/libs/stitching/GameConfig.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hm::stitching {
namespace {

namespace fs = std::filesystem;

bool yaml_equal(const YAML::Node& lhs, const YAML::Node& rhs) {
  if (lhs.IsDefined() != rhs.IsDefined())
    return false;
  if (!lhs.IsDefined())
    return true;
  return YAML::Dump(lhs) == YAML::Dump(rhs);
}

absl::Status fsync_directory(const fs::path& path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (descriptor < 0)
    return absl::InternalError("Unable to open game directory for fsync: " + std::string(std::strerror(errno)));
  const int result = ::fsync(descriptor);
  const std::string message = result == 0 ? std::string() : std::strerror(errno);
  ::close(descriptor);
  if (result != 0)
    return absl::InternalError("Unable to fsync game directory: " + message);
  return absl::OkStatus();
}

} // namespace

GameConfigLock::~GameConfigLock() {
  if (descriptor_ >= 0) {
    ::flock(descriptor_, LOCK_UN);
    ::close(descriptor_);
  }
}

absl::StatusOr<std::unique_ptr<GameConfigLock>> GameConfigLock::Acquire(const fs::path& game_dir) {
  std::error_code error;
  if (!fs::is_directory(game_dir, error) || error)
    return absl::NotFoundError("Cannot lock config outside an existing game directory: " + game_dir.string());
  const fs::path path = game_dir / ".hmstream-config.lock";
  const int descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (descriptor < 0)
    return absl::InternalError("Unable to open game config lock: " + std::string(std::strerror(errno)));
  if (::flock(descriptor, LOCK_EX) != 0) {
    const std::string message = std::strerror(errno);
    ::close(descriptor);
    return absl::InternalError("Unable to lock game config: " + message);
  }
  return std::unique_ptr<GameConfigLock>(new GameConfigLock(descriptor));
}

absl::Status publish_game_config(const fs::path& game_dir, const std::string& contents) {
  std::string pattern = (game_dir / ".hmstream-config-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const int descriptor = ::mkstemp(writable.data());
  if (descriptor < 0)
    return absl::InternalError("Unable to create temporary game config: " + std::string(std::strerror(errno)));
  const fs::path temporary(writable.data());
  auto fail = [&](const std::string& message) {
    ::close(descriptor);
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return absl::InternalError(message);
  };
  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t count = ::write(descriptor, contents.data() + offset, contents.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return fail("Unable to write temporary game config: " + std::string(std::strerror(errno)));
    offset += static_cast<size_t>(count);
  }
  if (::fsync(descriptor) != 0)
    return fail("Unable to fsync temporary game config: " + std::string(std::strerror(errno)));
  if (::close(descriptor) != 0) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return absl::InternalError("Unable to close temporary game config: " + std::string(std::strerror(errno)));
  }
  std::error_code error;
  fs::rename(temporary, game_dir / "config.yaml", error);
  if (error) {
    const std::string message = error.message();
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return absl::InternalError("Unable to atomically publish game config: " + message);
  }
  return fsync_directory(game_dir);
}

absl::StatusOr<std::optional<YAML::Node>> load_game_config_file(const fs::path& config_path) {
  auto lock = GameConfigTransactionLock::Acquire(config_path.parent_path());
  if (!lock.ok())
    return lock.status();
  std::error_code error;
  const bool exists = fs::exists(config_path, error);
  if (error)
    return absl::InternalError("Unable to inspect game config: " + error.message());
  if (!exists)
    return std::nullopt;
  if (!fs::is_regular_file(config_path, error) || error)
    return absl::FailedPreconditionError("Game config is not a regular file: " + config_path.string());
  try {
    return std::optional<YAML::Node>(YAML::LoadFile(config_path.string()));
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError("Unable to load game config: " + std::string(exception.what()));
  }
}

YAML::Node apply_game_config_diff(const YAML::Node& baseline, const YAML::Node& desired, const YAML::Node& latest) {
  const bool empty_map_baseline = !baseline.IsDefined() || baseline.IsNull();
  if ((baseline.IsMap() || empty_map_baseline) && desired.IsMap()) {
    YAML::Node result = latest.IsMap() ? YAML::Clone(latest) : YAML::Node(YAML::NodeType::Map);
    if (baseline.IsMap()) {
      for (const auto& pair : baseline) {
        const std::string key = pair.first.as<std::string>();
        if (!desired[key].IsDefined())
          result.remove(key);
      }
    }
    for (const auto& pair : desired) {
      const std::string key = pair.first.as<std::string>();
      const YAML::Node old_value = baseline.IsMap() ? baseline[key] : YAML::Node(YAML::NodeType::Undefined);
      if (!old_value.IsDefined()) {
        result[key] = YAML::Clone(pair.second);
      } else if (!yaml_equal(old_value, pair.second)) {
        result[key] = apply_game_config_diff(old_value, pair.second, result[key]);
      }
    }
    return result;
  }
  return yaml_equal(baseline, desired) ? YAML::Clone(latest) : YAML::Clone(desired);
}

} // namespace hm::stitching
