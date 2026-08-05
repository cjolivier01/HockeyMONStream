#include "hstream/src/libs/stitching/GameConfig.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
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

bool sequence_contains(const YAML::Node& sequence, const YAML::Node& value) {
  if (!sequence.IsSequence())
    return false;
  for (const auto& item : sequence) {
    if (yaml_equal(item, value))
      return true;
  }
  return false;
}

YAML::Node map_value(const YAML::Node& map, const std::string& key) {
  if (map.IsMap()) {
    for (const auto& pair : map) {
      if (pair.first.IsScalar() && pair.first.as<std::string>() == key)
        return pair.second;
    }
  }
  return YAML::Node(YAML::NodeType::Undefined);
}

YAML::Node merge_rollback_impl(const YAML::Node& baseline, const YAML::Node& desired, const YAML::Node& latest) {
  if (yaml_equal(baseline, desired))
    return YAML::Clone(latest);
  if (yaml_equal(latest, baseline))
    return YAML::Clone(desired);

  const bool empty_map_baseline = !baseline.IsDefined() || baseline.IsNull();
  if ((baseline.IsMap() || empty_map_baseline) && desired.IsMap()) {
    if (latest.IsDefined() && !latest.IsNull() && !latest.IsMap())
      return YAML::Clone(latest);
    YAML::Node result = latest.IsMap() ? YAML::Clone(latest) : YAML::Node(YAML::NodeType::Map);
    if (baseline.IsMap()) {
      for (const auto& pair : baseline) {
        const std::string key = pair.first.as<std::string>();
        if (!map_value(desired, key).IsDefined() && yaml_equal(map_value(result, key), pair.second))
          result.remove(key);
      }
    }
    for (const auto& pair : desired) {
      const std::string key = pair.first.as<std::string>();
      const YAML::Node old_value = map_value(baseline, key);
      if (!old_value.IsDefined() || !yaml_equal(old_value, pair.second))
        result[key] = merge_rollback_impl(old_value, pair.second, map_value(result, key));
    }
    return result;
  }

  const bool empty_sequence_baseline = !baseline.IsDefined() || baseline.IsNull();
  if ((baseline.IsSequence() || empty_sequence_baseline) && desired.IsSequence() && latest.IsSequence()) {
    std::vector<YAML::Node> merged;
    for (const auto& item : latest) {
      const bool rollback_removal = sequence_contains(baseline, item) && !sequence_contains(desired, item);
      if (!rollback_removal && std::none_of(merged.begin(), merged.end(), [&](const YAML::Node& value) {
            return yaml_equal(value, item);
          })) {
        merged.push_back(YAML::Clone(item));
      }
    }

    // Insert restored entries around the desired sequence's nearest surviving
    // anchors. This preserves the relative order of every entry already in
    // latest instead of moving concurrent additions behind older entries.
    for (size_t desired_index = desired.size(); desired_index > 0; --desired_index) {
      const YAML::Node item = desired[desired_index - 1];
      if (sequence_contains(baseline, item) ||
          std::any_of(merged.begin(), merged.end(), [&](const YAML::Node& value) { return yaml_equal(value, item); })) {
        continue;
      }

      auto insertion = merged.begin();
      bool found_anchor = false;
      for (size_t next = desired_index; next < desired.size() && !found_anchor; ++next) {
        insertion = std::find_if(
            merged.begin(), merged.end(), [&](const YAML::Node& value) { return yaml_equal(value, desired[next]); });
        found_anchor = insertion != merged.end();
      }
      if (!found_anchor) {
        for (size_t previous = desired_index - 1; previous > 0; --previous) {
          auto anchor = std::find_if(merged.begin(), merged.end(), [&](const YAML::Node& value) {
            return yaml_equal(value, desired[previous - 1]);
          });
          if (anchor != merged.end()) {
            insertion = std::next(anchor);
            found_anchor = true;
            break;
          }
        }
      }
      if (!found_anchor)
        insertion = merged.begin();
      merged.insert(insertion, YAML::Clone(item));
    }

    YAML::Node result(YAML::NodeType::Sequence);
    for (const auto& item : merged)
      result.push_back(item);
    return result;
  }

  return YAML::Clone(latest);
}

struct ScopedRinkLock {
  int descriptor{-1};
  ~ScopedRinkLock() {
    if (descriptor >= 0) {
      ::flock(descriptor, LOCK_UN);
      ::close(descriptor);
    }
  }
};

absl::Status fsync_path(const fs::path& path, bool directory = false) {
  const int flags = O_RDONLY | O_CLOEXEC | (directory ? O_DIRECTORY : 0);
  const int descriptor = ::open(path.c_str(), flags);
  if (descriptor < 0)
    return absl::InternalError("Unable to open artifact for fsync: " + path.string());
  const int result = ::fsync(descriptor);
  const std::string message = result == 0 ? std::string() : std::strerror(errno);
  ::close(descriptor);
  if (result != 0)
    return absl::InternalError("Unable to fsync artifact " + path.string() + ": " + message);
  return absl::OkStatus();
}

absl::Status write_transaction_file(const fs::path& path, const std::string& contents) {
  std::ofstream output(path, std::ios::out | std::ios::trunc);
  if (!output)
    return absl::InternalError("Unable to write rink transaction file: " + path.string());
  output << contents;
  output.flush();
  if (!output)
    return absl::InternalError("Unable to flush rink transaction file: " + path.string());
  output.close();
  return fsync_path(path);
}

bool is_rink_artifact_name(const std::string& name) {
  static const std::regex mask_pattern(R"(^rink_mask_(0|[1-9][0-9]*)[.]png$)");
  return name == "config.yaml" || std::regex_match(name, mask_pattern);
}

absl::StatusOr<std::unique_ptr<ScopedRinkLock>> lock_rink_transactions(const fs::path& root) {
  const fs::path path = root / ".hmstream-rink.lock";
  const int descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (descriptor < 0)
    return absl::InternalError("Unable to open rink transaction lock: " + std::string(std::strerror(errno)));
  if (::flock(descriptor, LOCK_EX) != 0) {
    const std::string message = std::strerror(errno);
    ::close(descriptor);
    return absl::InternalError("Unable to lock rink transaction: " + message);
  }
  auto lock = std::make_unique<ScopedRinkLock>();
  lock->descriptor = descriptor;
  return lock;
}

absl::StatusOr<std::string> read_rink_transaction_state(const fs::path& transaction) {
  const fs::path state_path = transaction / "state";
  std::error_code error;
  const bool exists = fs::exists(state_path, error);
  if (error)
    return absl::InternalError("Unable to inspect rink transaction state: " + error.message());
  if (!exists)
    return std::string("UNPREPARED");
  const int descriptor = ::open(state_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0)
    return absl::FailedPreconditionError("Unable to open durable rink transaction state");
  struct StateFileCleanup {
    int descriptor;
    ~StateFileCleanup() {
      ::close(descriptor);
    }
  } cleanup{descriptor};
  struct stat metadata{};
  if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
      metadata.st_size > 10) {
    return absl::FailedPreconditionError("Invalid durable rink transaction state file");
  }
  std::string contents(static_cast<size_t>(metadata.st_size), '\0');
  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t count = ::read(descriptor, contents.data() + offset, contents.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return absl::FailedPreconditionError("Unable to read durable rink transaction state");
    offset += static_cast<size_t>(count);
  }
  if (contents == "PREPARED\n")
    return std::string("PREPARED");
  if (contents == "COMMITTED\n")
    return std::string("COMMITTED");
  return absl::FailedPreconditionError("Invalid durable rink transaction state contents");
}

absl::StatusOr<std::set<std::string>> read_rink_manifest(const fs::path& transaction) {
  const fs::path path = transaction / "new-files";
  std::error_code error;
  if (!fs::is_regular_file(path, error) || error)
    return absl::FailedPreconditionError("Prepared rink transaction has no readable new-files manifest");
  if (fs::file_size(path, error) > 64 * 1024 || error)
    return absl::FailedPreconditionError("Prepared rink transaction manifest is too large");
  std::ifstream input(path);
  if (!input)
    return absl::FailedPreconditionError("Unable to open prepared rink transaction manifest");
  std::set<std::string> names;
  std::string name;
  while (input >> name) {
    if (fs::path(name).filename() != name || !is_rink_artifact_name(name) || !names.insert(name).second)
      return absl::InvalidArgumentError("Invalid rink transaction filename: " + name);
  }
  if (!input.eof() || !names.count("config.yaml") || names.size() < 2)
    return absl::FailedPreconditionError("Prepared rink transaction manifest is incomplete");
  return names;
}

absl::Status recover_rink_transactions_locked(const fs::path& root) {
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(root, error)) {
    if (error)
      return absl::InternalError("Unable to inspect rink transactions: " + error.message());
    const std::string directory_name = entry.path().filename().string();
    if (!entry.is_directory(error) || error || directory_name.rfind(".hmstream-rink-", 0) != 0) {
      error.clear();
      continue;
    }
    const fs::path transaction = entry.path();
    auto state = read_rink_transaction_state(transaction);
    if (!state.ok())
      return state.status();
    if (*state == "PREPARED") {
      auto manifest = read_rink_manifest(transaction);
      if (!manifest.ok())
        return manifest.status();
      const fs::path previous = transaction / "previous";
      std::vector<fs::path> backups;
      const bool previous_exists = fs::exists(previous, error);
      if (error)
        return absl::InternalError("Unable to inspect rink transaction backup directory: " + error.message());
      if (previous_exists) {
        if (!fs::is_directory(previous, error) || error)
          return absl::FailedPreconditionError("Rink transaction backup is not a directory");
        for (const auto& old : fs::directory_iterator(previous, error)) {
          if (error)
            return absl::InternalError("Unable to inspect rink transaction backup: " + error.message());
          const std::string old_name = old.path().filename().string();
          if (!old.is_regular_file(error) || error || !is_rink_artifact_name(old_name))
            return absl::InvalidArgumentError("Invalid rink transaction backup: " + old_name);
          backups.push_back(old.path());
        }
      }
      for (const std::string& name : *manifest) {
        fs::remove(root / name, error);
        if (error)
          return absl::InternalError("Unable to remove interrupted rink artifact: " + error.message());
      }
      size_t restored = 0;
      for (const fs::path& old : backups) {
        const fs::path destination = root / old.filename();
        fs::copy_file(old, destination, fs::copy_options::overwrite_existing, error);
        if (error)
          return absl::InternalError("Unable to restore interrupted rink artifact: " + error.message());
        auto status = fsync_path(destination);
        if (!status.ok())
          return status;
        ++restored;
        if (const char* fail_after = std::getenv("HM_TEST_RINK_ROLLBACK_FAIL_AFTER");
            fail_after != nullptr && restored == static_cast<size_t>(std::strtoull(fail_after, nullptr, 10))) {
          return absl::InternalError("Injected rink rollback interruption");
        }
      }
      error.clear();
      auto status = fsync_path(root, true);
      if (!status.ok())
        return status;
    }
    fs::remove_all(transaction, error);
    if (error)
      return absl::InternalError("Unable to clean rink transaction: " + error.message());
  }
  return fsync_path(root, true);
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

struct GameConfigTransactionLock::State {
  std::unique_ptr<GameConfigLock> config;
  std::unique_ptr<ScopedRinkLock> rink;
};

GameConfigTransactionLock::GameConfigTransactionLock(std::unique_ptr<State> state) : state_(std::move(state)) {}

GameConfigTransactionLock::~GameConfigTransactionLock() = default;

absl::StatusOr<std::unique_ptr<GameConfigTransactionLock>> GameConfigTransactionLock::Acquire(
    const fs::path& game_dir) {
  auto config = GameConfigLock::Acquire(game_dir);
  if (!config.ok())
    return config.status();
  auto rink = lock_rink_transactions(game_dir);
  if (!rink.ok())
    return rink.status();
  auto recovery = recover_rink_transactions_locked(game_dir);
  if (!recovery.ok())
    return recovery;
  auto state = std::make_unique<State>();
  state->config = std::move(*config);
  state->rink = std::move(*rink);
  return std::unique_ptr<GameConfigTransactionLock>(new GameConfigTransactionLock(std::move(state)));
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

absl::StatusOr<size_t> publish_game_config_without_rink_masks(const fs::path& game_dir, const std::string& contents) {
  std::error_code error;
  std::vector<fs::path> masks;
  for (const auto& entry : fs::directory_iterator(game_dir, error)) {
    if (error)
      return absl::InternalError("Unable to inspect rink masks: " + error.message());
    const std::string name = entry.path().filename().string();
    if (name != "config.yaml" && is_rink_artifact_name(name)) {
      if (!entry.is_regular_file(error) || error)
        return absl::FailedPreconditionError("Rink mask is not a regular file: " + entry.path().string());
      masks.push_back(entry.path());
    }
  }
  if (masks.empty()) {
    auto status = publish_game_config(game_dir, contents);
    if (!status.ok())
      return status;
    return 0;
  }

  std::string pattern = (game_dir / ".hmstream-rink-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  char* created = ::mkdtemp(writable.data());
  if (created == nullptr)
    return absl::InternalError("Unable to create rink invalidation staging directory");
  const fs::path staging(created);
  struct Cleanup {
    fs::path path;
    bool prepared{false};
    ~Cleanup() {
      if (prepared)
        return;
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{staging};
  if (::chmod(staging.c_str(), 0700) != 0)
    return absl::InternalError("Unable to protect rink invalidation staging directory");

  auto status = write_transaction_file(staging / "config.yaml", contents);
  if (!status.ok())
    return status;
  if (::chmod((staging / "config.yaml").c_str(), 0600) != 0)
    return absl::InternalError("Unable to protect staged game config: " + std::string(std::strerror(errno)));
  status = fsync_path(staging / "config.yaml");
  if (!status.ok())
    return status;
  const fs::path previous = staging / "previous";
  fs::create_directory(previous, error);
  if (error)
    return absl::InternalError("Unable to create rink invalidation rollback directory: " + error.message());

  std::vector<fs::path> old_files = masks;
  const fs::path current_config = game_dir / "config.yaml";
  if (fs::exists(current_config, error)) {
    if (error || !fs::is_regular_file(current_config, error) || error)
      return absl::FailedPreconditionError("Game config is not a regular file: " + current_config.string());
    old_files.push_back(current_config);
  } else if (error) {
    return absl::InternalError("Unable to inspect game config: " + error.message());
  }
  for (const fs::path& old : old_files) {
    fs::copy_file(old, previous / old.filename(), fs::copy_options::overwrite_existing, error);
    if (error)
      return absl::InternalError("Unable to preserve old rink artifact: " + error.message());
    status = fsync_path(previous / old.filename());
    if (!status.ok())
      return status;
  }

  std::set<std::string> published_names{"config.yaml"};
  for (const fs::path& mask : masks)
    published_names.insert(mask.filename().string());
  std::ostringstream manifest;
  for (const std::string& name : published_names)
    manifest << name << '\n';
  status = write_transaction_file(staging / "new-files", manifest.str());
  if (!status.ok())
    return status;
  status = fsync_path(previous, true);
  if (!status.ok())
    return status;
  status = fsync_path(staging, true);
  if (!status.ok())
    return status;
  status = write_transaction_file(staging / "state", "PREPARED\n");
  if (!status.ok())
    return status;
  status = fsync_path(staging, true);
  if (!status.ok())
    return status;
  status = fsync_path(game_dir, true);
  if (!status.ok())
    return status;
  cleanup.prepared = true;

  auto rollback_error = [&](const std::string& message) -> absl::StatusOr<size_t> {
    const auto rollback = recover_rink_transactions_locked(game_dir);
    if (!rollback.ok())
      return absl::InternalError(message + "; rollback also failed: " + std::string(rollback.message()));
    return absl::InternalError(message);
  };
  size_t removed = 0;
  for (const std::string& name : published_names) {
    fs::remove(game_dir / name, error);
    if (error)
      return rollback_error("Unable to remove old rink artifact: " + error.message());
    if (name != "config.yaml")
      ++removed;
    if (const char* fail_after = std::getenv("HM_TEST_RINK_INVALIDATION_FAIL_AFTER_REMOVE");
        fail_after != nullptr && removed == static_cast<size_t>(std::strtoull(fail_after, nullptr, 10))) {
      return rollback_error("Injected rink invalidation failure");
    }
  }
  fs::rename(staging / "config.yaml", game_dir / "config.yaml", error);
  if (error)
    return rollback_error("Unable to publish invalidated rink config: " + error.message());
  status = fsync_path(game_dir / "config.yaml");
  if (!status.ok())
    return rollback_error(std::string(status.message()));
  status = fsync_path(game_dir, true);
  if (!status.ok())
    return rollback_error(std::string(status.message()));
  status = write_transaction_file(staging / "state.committed", "COMMITTED\n");
  if (!status.ok())
    return rollback_error(std::string(status.message()));
  fs::rename(staging / "state.committed", staging / "state", error);
  if (error)
    return rollback_error("Unable to commit rink invalidation: " + error.message());
  status = fsync_path(staging, true);
  if (!status.ok())
    return status;
  fs::remove_all(staging, error);
  if (error)
    return absl::InternalError("Unable to clean committed rink invalidation: " + error.message());
  status = fsync_path(game_dir, true);
  if (!status.ok())
    return status;
  return removed;
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
        result[key] = pair.second.IsMap()
            ? apply_game_config_diff(YAML::Node(YAML::NodeType::Undefined), pair.second, result[key])
            : YAML::Clone(pair.second);
      } else if (!yaml_equal(old_value, pair.second)) {
        result[key] = apply_game_config_diff(old_value, pair.second, result[key]);
      }
    }
    return result;
  }
  return yaml_equal(baseline, desired) ? YAML::Clone(latest) : YAML::Clone(desired);
}

YAML::Node merge_game_config_rollback(const YAML::Node& baseline, const YAML::Node& desired, const YAML::Node& latest) {
  return merge_rollback_impl(baseline, desired, latest);
}

} // namespace hm::stitching
