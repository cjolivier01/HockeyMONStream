#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/TransactionState.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
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
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hm::stitching {
namespace {

namespace fs = std::filesystem;

absl::StatusOr<std::vector<fs::directory_entry>> directory_entries(
    const fs::path& directory,
    const std::string& description) {
  std::error_code error;
  fs::directory_iterator iterator(directory, error);
  if (error)
    return absl::InternalError("Unable to inspect " + description + ": " + error.message());
  std::vector<fs::directory_entry> entries;
  const fs::directory_iterator end;
  while (iterator != end) {
    entries.push_back(*iterator);
    iterator.increment(error);
    if (error)
      return absl::InternalError("Unable to inspect " + description + ": " + error.message());
  }
  return entries;
}

struct ProcessIdentity {
  pid_t process_id;
  uint64_t start_time;
  std::string boot_id;
};

struct ProcessMetadata {
  uint64_t start_time;
  char state;
};

absl::StatusOr<std::string> current_boot_id() {
  std::ifstream input("/proc/sys/kernel/random/boot_id");
  std::string boot_id;
  if (!input || !std::getline(input, boot_id) || boot_id.empty() ||
      boot_id.find_first_of(": \t\r\n") != std::string::npos) {
    return absl::InternalError("Unable to read the current Linux boot identity");
  }
  return boot_id;
}

absl::StatusOr<ProcessMetadata> process_metadata(pid_t process_id) {
  std::ifstream input("/proc/" + std::to_string(process_id) + "/stat");
  if (!input)
    return absl::NotFoundError("Live stitched-output owner process is not running");
  std::string stat;
  std::getline(input, stat);
  const size_t command_end = stat.rfind(')');
  if (command_end == std::string::npos || command_end + 2 >= stat.size())
    return absl::InternalError("Unable to parse live stitched-output owner process metadata");
  std::istringstream fields(stat.substr(command_end + 2));
  std::string value;
  char state = 0;
  for (int field = 3; field <= 22; ++field) {
    if (!(fields >> value))
      return absl::InternalError("Unable to parse live stitched-output owner process start time");
    if (field == 3) {
      if (value.size() != 1)
        return absl::InternalError("Invalid live stitched-output owner process state");
      state = value.front();
    }
  }
  uint64_t start_time = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), start_time);
  if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size())
    return absl::InternalError("Invalid live stitched-output owner process start time");
  return ProcessMetadata{start_time, state};
}

absl::StatusOr<ProcessIdentity> parse_process_identity(std::string_view identity) {
  const size_t process_separator = identity.find(':');
  if (process_separator == std::string_view::npos || process_separator == 0 ||
      process_separator + 1 == identity.size()) {
    return absl::InvalidArgumentError("Invalid live stitched-output owner process identity");
  }
  const size_t boot_separator = identity.find(':', process_separator + 1);
  const size_t start_end = boot_separator == std::string_view::npos ? identity.size() : boot_separator;
  if (start_end == process_separator + 1 ||
      (boot_separator != std::string_view::npos &&
       (boot_separator + 1 == identity.size() || identity.find(':', boot_separator + 1) != std::string_view::npos))) {
    return absl::InvalidArgumentError("Invalid live stitched-output owner process identity");
  }
  pid_t process_id = 0;
  uint64_t start_time = 0;
  const auto process = std::from_chars(identity.data(), identity.data() + process_separator, process_id);
  const auto start = std::from_chars(identity.data() + process_separator + 1, identity.data() + start_end, start_time);
  if (process.ec != std::errc() || process.ptr != identity.data() + process_separator || process_id <= 0 ||
      start.ec != std::errc() || start.ptr != identity.data() + start_end) {
    return absl::InvalidArgumentError("Invalid live stitched-output owner process identity");
  }
  const std::string boot_id =
      boot_separator == std::string_view::npos ? std::string() : std::string(identity.substr(boot_separator + 1));
  if (!boot_id.empty() && boot_id.find_first_of(" \t\r\n") != std::string::npos) {
    return absl::InvalidArgumentError("Invalid live stitched-output owner process identity");
  }
  return ProcessIdentity{process_id, start_time, boot_id};
}

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

absl::Status link_clone_or_copy_rollback_file(const fs::path& source, const fs::path& destination) {
  const char* force_portable_fallback = std::getenv("HM_TEST_RINK_DISABLE_LINK_CLONE");
  return snapshot_regular_file_for_rollback(
      source, destination, force_portable_fallback != nullptr && std::string(force_portable_fallback) == "1");
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

absl::Status mark_rink_transaction_rolled_back(const fs::path& transaction) {
  const fs::path temporary = transaction / "state.rolled_back";
  auto status = write_transaction_file(temporary, "ROLLED_BACK\n");
  if (!status.ok())
    return status;
  std::error_code error;
  fs::rename(temporary, transaction / "state", error);
  if (error)
    return absl::InternalError("Unable to commit rink rollback state: " + error.message());
  return fsync_path(transaction, true);
}

bool is_rink_artifact_name(const std::string& name) {
  static const std::regex mask_pattern(R"(^rink_mask_(0|[1-9][0-9]*)[.]png$)");
  return name == "config.yaml" || name == "s.png" || std::regex_match(name, mask_pattern);
}

bool is_rink_mask_name(const std::string& name) {
  static const std::regex mask_pattern(R"(^rink_mask_(0|[1-9][0-9]*)[.]png$)");
  return std::regex_match(name, mask_pattern);
}

absl::StatusOr<std::unique_ptr<ScopedRinkLock>> lock_rink_transactions(const fs::path& root) {
  const fs::path path = root / ".hstream-rink.lock";
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
  const fs::file_status state_status = fs::symlink_status(state_path, error);
  if (error == std::errc::no_such_file_or_directory)
    error.clear();
  else if (error)
    return absl::InternalError("Unable to inspect rink transaction state: " + error.message());
  if (state_status.type() == fs::file_type::not_found)
    return std::string("UNPREPARED");
  const int descriptor = ::open(state_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
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
      metadata.st_size > 16) {
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
  if (contents == "ROLLED_BACK\n")
    return std::string("ROLLED_BACK");
  return absl::FailedPreconditionError("Invalid durable rink transaction state contents");
}

absl::StatusOr<std::set<std::string>> read_rink_manifest(const fs::path& transaction) {
  const fs::path path = transaction / "new-files";
  auto contents = read_bounded_regular_file_no_follow(path, 64 * 1024, "prepared rink transaction manifest");
  if (!contents.ok())
    return contents.status();
  std::istringstream input(*contents);
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
  auto opened_root = PinnedDirectory::Open(root, "rink transaction root");
  if (!opened_root.ok())
    return opened_root.status();
  PinnedDirectory root_directory = std::move(*opened_root);
  auto root_entries = directory_entries(root_directory.path(), "rink transactions");
  if (!root_entries.ok())
    return root_entries.status();
  for (const auto& entry : *root_entries) {
    const std::string directory_name = entry.path().filename().string();
    if (directory_name.rfind(".hstream-rink-", 0) != 0)
      continue;
    auto opened_transaction = root_directory.OpenChild(directory_name, "rink transaction directory");
    if (!opened_transaction.ok())
      return opened_transaction.status();
    if (!opened_transaction->has_value())
      continue;
    PinnedDirectory transaction_directory = std::move(**opened_transaction);
    const fs::path transaction = transaction_directory.path();
    auto state = read_rink_transaction_state(transaction);
    if (!state.ok())
      return state.status();
    if (*state == "PREPARED") {
      auto manifest = read_rink_manifest(transaction);
      if (!manifest.ok())
        return manifest.status();
      std::vector<fs::path> backups;
      std::optional<PinnedDirectory> previous_directory;
      auto opened_previous = transaction_directory.OpenChild("previous", "rink transaction backup directory");
      if (!opened_previous.ok())
        return opened_previous.status();
      if (opened_previous->has_value()) {
        previous_directory.emplace(std::move(**opened_previous));
        const fs::path previous = previous_directory->path();
        auto previous_entries = directory_entries(previous, "rink transaction backup");
        if (!previous_entries.ok())
          return previous_entries.status();
        for (const auto& old : *previous_entries) {
          const std::string old_name = old.path().filename().string();
          const bool is_regular = old.symlink_status(error).type() == fs::file_type::regular;
          if (error)
            return absl::InternalError("Unable to inspect rink transaction backup entry: " + error.message());
          if (!is_regular || !is_rink_artifact_name(old_name))
            return absl::InvalidArgumentError("Invalid rink transaction backup: " + old_name);
          backups.push_back(old.path());
        }
      }
      for (const std::string& name : *manifest) {
        fs::remove(root_directory.path() / name, error);
        if (error)
          return absl::InternalError("Unable to remove interrupted rink artifact: " + error.message());
      }
      size_t restored = 0;
      for (const fs::path& old : backups) {
        const fs::path destination = root_directory.path() / old.filename();
        auto restore = link_clone_or_copy_rollback_file(old, destination);
        if (!restore.ok())
          return absl::InternalError("Unable to restore interrupted rink artifact: " + std::string(restore.message()));
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
      auto status = fsync_path(root_directory.path(), true);
      if (!status.ok())
        return status;
      status = mark_rink_transaction_rolled_back(transaction);
      if (!status.ok())
        return status;
    }
    auto cleanup = remove_pinned_directory(root_directory, directory_name, transaction_directory);
    if (!cleanup.ok())
      return cleanup;
  }
  return fsync_path(root_directory.path(), true);
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
  const fs::path path = game_dir / ".hstream-config.lock";
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

absl::StatusOr<std::unique_ptr<GameConfigLock>> GameConfigLock::TryAcquire(const fs::path& game_dir) {
  std::error_code error;
  if (!fs::is_directory(game_dir, error) || error)
    return absl::NotFoundError("Cannot lock config outside an existing game directory: " + game_dir.string());
  const fs::path path = game_dir / ".hstream-config.lock";
  const int descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (descriptor < 0)
    return absl::InternalError("Unable to open game config lock: " + std::string(std::strerror(errno)));
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    const int lock_errno = errno;
    const std::string message = std::strerror(lock_errno);
    ::close(descriptor);
    if (lock_errno == EWOULDBLOCK || lock_errno == EAGAIN)
      return absl::UnavailableError("Game config is being updated");
    return absl::InternalError("Unable to lock game config: " + message);
  }
  return std::unique_ptr<GameConfigLock>(new GameConfigLock(descriptor));
}

absl::Status publish_game_config(const fs::path& game_dir, const std::string& contents) {
  return publish_named_file(game_dir / "config.yaml", contents);
}

absl::Status publish_named_file(const fs::path& path, const std::string& contents) {
  const fs::path parent = path.parent_path();
  if (parent.empty())
    return absl::InvalidArgumentError("Atomic publication requires a parent directory");
  std::string pattern = (parent / ("." + path.filename().string() + "-XXXXXX")).string();
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
  fs::rename(temporary, path, error);
  if (error) {
    const std::string message = error.message();
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return absl::InternalError("Unable to atomically publish game config: " + message);
  }
  return fsync_directory(parent);
}

absl::StatusOr<size_t> publish_game_config_without_rink_masks(
    const fs::path& game_dir,
    const std::string& contents,
    bool remove_stitched_snapshot) {
  std::error_code error;
  std::vector<fs::path> invalidated_artifacts;
  auto game_entries = directory_entries(game_dir, "rink masks");
  if (!game_entries.ok())
    return game_entries.status();
  for (const auto& entry : *game_entries) {
    const std::string name = entry.path().filename().string();
    if (is_rink_mask_name(name) || (remove_stitched_snapshot && name == "s.png")) {
      if (!entry.is_regular_file(error) || error)
        return absl::FailedPreconditionError("Rink artifact is not a regular file: " + entry.path().string());
      invalidated_artifacts.push_back(entry.path());
    }
  }
  if (invalidated_artifacts.empty()) {
    auto status = publish_game_config(game_dir, contents);
    if (!status.ok())
      return status;
    return 0;
  }

  std::string pattern = (game_dir / ".hstream-rink-XXXXXX").string();
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

  std::vector<fs::path> old_files = invalidated_artifacts;
  const fs::path current_config = game_dir / "config.yaml";
  if (fs::exists(current_config, error)) {
    if (error || !fs::is_regular_file(current_config, error) || error)
      return absl::FailedPreconditionError("Game config is not a regular file: " + current_config.string());
    old_files.push_back(current_config);
  } else if (error) {
    return absl::InternalError("Unable to inspect game config: " + error.message());
  }
  for (const fs::path& old : old_files) {
    auto preserve = link_clone_or_copy_rollback_file(old, previous / old.filename());
    if (!preserve.ok())
      return preserve;
    status = fsync_path(previous / old.filename());
    if (!status.ok())
      return status;
  }

  std::set<std::string> published_names{"config.yaml"};
  for (const fs::path& artifact : invalidated_artifacts)
    published_names.insert(artifact.filename().string());
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
  status = publish_transaction_state(staging, "PREPARED\n");
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

namespace {

absl::Status validate_stitching_generation_owner_impl(
    const YAML::Node& config,
    const std::string& expected_invalidation_id,
    bool allow_completed) {
  if (expected_invalidation_id.empty())
    return absl::OkStatus();
  try {
    YAML::Node calibration;
    if (config && config.IsMap()) {
      const YAML::Node ui = config["hstream_ui"];
      if (ui && ui.IsMap())
        calibration = ui["stitching_calibration"];
    }
    const std::string current_invalidation_id =
        calibration && calibration["invalidation_id"] && calibration["invalidation_id"].IsScalar()
        ? calibration["invalidation_id"].as<std::string>()
        : std::string();
    const std::string current_status = calibration && calibration["status"] && calibration["status"].IsScalar()
        ? calibration["status"].as<std::string>()
        : std::string();
    const bool status_matches = current_status == "pending" || (allow_completed && current_status == "complete");
    if (current_invalidation_id != expected_invalidation_id || !status_matches)
      return absl::AbortedError("Stitching invalidation was superseded");
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError("Unable to validate stitching invalidation: " + std::string(exception.what()));
  }
  return absl::OkStatus();
}

absl::Status validate_stitching_generation_owner_file_locked_impl(
    const fs::path& config_path,
    const std::string& expected_invalidation_id,
    bool allow_completed) {
  if (expected_invalidation_id.empty())
    return absl::OkStatus();
  try {
    const YAML::Node config = fs::is_regular_file(config_path) ? YAML::LoadFile(config_path.string()) : YAML::Node();
    return validate_stitching_generation_owner_impl(config, expected_invalidation_id, allow_completed);
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to load stitching invalidation for validation: " + std::string(exception.what()));
  }
}

} // namespace

absl::Status validate_pending_stitching_invalidation(
    const YAML::Node& config,
    const std::string& expected_invalidation_id) {
  return validate_stitching_generation_owner_impl(config, expected_invalidation_id, /*allow_completed=*/false);
}

absl::Status validate_stitching_generation_owner(
    const YAML::Node& config,
    const std::string& expected_invalidation_id) {
  return validate_stitching_generation_owner_impl(config, expected_invalidation_id, /*allow_completed=*/true);
}

absl::Status validate_pending_stitching_invalidation_file_locked(
    const fs::path& config_path,
    const std::string& expected_invalidation_id) {
  return validate_stitching_generation_owner_file_locked_impl(
      config_path, expected_invalidation_id, /*allow_completed=*/false);
}

absl::Status validate_stitching_generation_owner_file_locked(
    const fs::path& config_path,
    const std::string& expected_invalidation_id) {
  return validate_stitching_generation_owner_file_locked_impl(
      config_path, expected_invalidation_id, /*allow_completed=*/true);
}

absl::StatusOr<std::string> current_live_stitched_output_owner_process() {
  auto metadata = process_metadata(::getpid());
  if (!metadata.ok())
    return metadata.status();
  auto boot_id = current_boot_id();
  if (!boot_id.ok())
    return boot_id.status();
  return std::to_string(::getpid()) + ":" + std::to_string(metadata->start_time) + ":" + *boot_id;
}

absl::StatusOr<bool> live_stitched_output_owner_process_is_active(std::string_view identity) {
  auto parsed = parse_process_identity(identity);
  if (!parsed.ok())
    return parsed.status();
  // A legacy pid:start identity cannot distinguish a restarted system and
  // therefore cannot retain publication authority.
  if (parsed->boot_id.empty())
    return false;
  auto boot_id = current_boot_id();
  if (!boot_id.ok())
    return boot_id.status();
  if (*boot_id != parsed->boot_id)
    return false;
  auto metadata = process_metadata(parsed->process_id);
  if (absl::IsNotFound(metadata.status()))
    return false;
  if (!metadata.ok())
    return metadata.status();
  if (metadata->state == 'Z' || metadata->state == 'X' || metadata->state == 'x')
    return false;
  return metadata->start_time == parsed->start_time;
}

absl::StatusOr<bool> live_stitched_output_authorization_is_active(const YAML::Node& config) {
  try {
    if (config && config.IsDefined() && !config.IsNull() && !config.IsMap())
      return absl::InvalidArgumentError("Game config must be a map");
    const YAML::Node rink = config && config.IsMap() ? config["rink"] : YAML::Node(YAML::NodeType::Undefined);
    if (!rink || !rink.IsDefined() || rink.IsNull())
      return false;
    if (!rink.IsMap())
      return absl::InvalidArgumentError("Rink config must be a map");
    const YAML::Node generation = rink["stitched_output_pending_generation"];
    const YAML::Node authorization = rink["stitched_output_pending_authorization_id"];
    const YAML::Node owner = rink["stitched_output_pending_owner_process"];
    const bool has_generation = generation && generation.IsDefined() && !generation.IsNull();
    const bool has_authorization = authorization && authorization.IsDefined() && !authorization.IsNull();
    const bool has_owner = owner && owner.IsDefined() && !owner.IsNull();
    if (!has_generation && !has_authorization)
      return false;
    if (!has_generation || !has_authorization || !generation.IsScalar() || !authorization.IsScalar() ||
        generation.as<std::string>().empty() || authorization.as<std::string>().empty()) {
      return absl::InvalidArgumentError("Pending stitched-output authorization is incomplete");
    }
    // Pending epochs from versions without process ownership cannot safely
    // retain authority after a restart.
    if (!has_owner)
      return false;
    if (!owner.IsScalar() || owner.as<std::string>().empty())
      return absl::InvalidArgumentError("Pending stitched-output owner process must be a nonempty scalar");
    return live_stitched_output_owner_process_is_active(owner.as<std::string>());
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to validate live stitched-output authorization: " + std::string(exception.what()));
  }
}

absl::Status validate_no_pending_live_stitched_output_authorization_file_locked(const fs::path& config_path) {
  try {
    const YAML::Node config = fs::is_regular_file(config_path) ? YAML::LoadFile(config_path.string()) : YAML::Node();
    auto active = live_stitched_output_authorization_is_active(config);
    if (!active.ok())
      return active.status();
    return *active ? absl::AbortedError("Live stitched-output authorization prevents Hugin artifact replacement")
                   : absl::OkStatus();
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to validate live stitched-output authorization: " + std::string(exception.what()));
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
