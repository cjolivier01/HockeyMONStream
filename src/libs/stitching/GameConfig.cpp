#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/stitching/TransactionState.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
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
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hm::stitching {
namespace {

constexpr std::string_view kOwnedDirectoryMarkerName = "journal_version";
constexpr std::string_view kOwnedDirectoryMarkerContents = "2\n";

absl::Status validate_projection_view(const StitchProjectionFraming& framing) {
  for (double angle : framing.rotation_degrees) {
    if (!std::isfinite(angle) || std::abs(angle) > 180.0)
      return absl::InvalidArgumentError("projection_framing.rotation_degrees must contain angles in [-180, 180]");
  }
  for (double edge : framing.crop) {
    if (!std::isfinite(edge) || edge < 0.0 || edge > 1.0)
      return absl::InvalidArgumentError("projection_framing.crop must contain fractions in [0, 1]");
  }
  if (framing.crop[0] >= framing.crop[1] || framing.crop[2] >= framing.crop[3])
    return absl::InvalidArgumentError("projection_framing.crop requires left < right and top < bottom");
  if (framing.auto_crop && framing.crop != StitchProjectionFraming{}.crop)
    return absl::InvalidArgumentError("projection_framing.crop cannot be combined with auto_crop");
  return absl::OkStatus();
}

template <size_t N>
absl::Status read_framing_array(const YAML::Node& framing, const char* key, std::array<double, N>& result) {
  const YAML::Node node = framing[key];
  if (!node || node.IsNull())
    return absl::OkStatus();
  if (!node.IsSequence() || node.size() != N)
    return absl::InvalidArgumentError(std::string("projection_framing.") + key + " has the wrong number of values");
  for (size_t index = 0; index < N; ++index)
    result[index] = node[index].as<double>();
  return absl::OkStatus();
}

void write_projection_view(YAML::Node node, const StitchProjectionFraming& framing) {
  node["rotation_degrees"] = std::vector<double>(framing.rotation_degrees.begin(), framing.rotation_degrees.end());
  node["crop"] = std::vector<double>(framing.crop.begin(), framing.crop.end());
}

absl::StatusOr<std::vector<double>> parse_projection_parameter_sequence(
    const YAML::Node& node,
    StitchProjection projection) {
  if (!node || !node.IsDefined() || node.IsNull())
    return DefaultStitchProjectionParameters(projection);
  if (!node.IsSequence()) {
    return absl::InvalidArgumentError(
        "stitching.projection_parameters." + std::string(StitchProjectionName(projection)) + " must be a sequence");
  }
  std::vector<double> parameters;
  parameters.reserve(node.size());
  try {
    for (const YAML::Node& value : node) {
      if (!value.IsScalar()) {
        return absl::InvalidArgumentError("stitching projection parameters must contain only numeric scalar values");
      }
      parameters.push_back(value.as<double>());
    }
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to parse stitching projection parameters: " + std::string(exception.what()));
  }
  const absl::Status validation = ValidateStitchProjectionParameters(projection, parameters);
  if (!validation.ok())
    return validation;
  return parameters;
}

absl::Status validate_projection_parameter_map(const YAML::Node& parameters) {
  if (!parameters || !parameters.IsDefined() || parameters.IsNull())
    return absl::OkStatus();
  if (!parameters.IsMap())
    return absl::InvalidArgumentError("stitching.projection_parameters must be a map");
  try {
    for (const auto& entry : parameters) {
      if (!entry.first.IsScalar())
        return absl::InvalidArgumentError("stitching.projection_parameters keys must be projection names");
      auto parsed_projection = ParseStitchProjection(entry.first.as<std::string>());
      if (!parsed_projection.ok())
        return parsed_projection.status();
      const StitchProjection projection = *parsed_projection;
      if (entry.first.as<std::string>() != StitchProjectionName(projection)) {
        return absl::InvalidArgumentError(
            "stitching.projection_parameters keys must use canonical projection names; use \"" +
            std::string(StitchProjectionName(projection)) + "\"");
      }
      if (StitchProjectionParameters(projection).empty()) {
        return absl::InvalidArgumentError(
            "stitching projection \"" + std::string(StitchProjectionName(projection)) +
            "\" does not accept projection parameters");
      }
      auto parsed = parse_projection_parameter_sequence(entry.second, projection);
      if (!parsed.ok())
        return parsed.status();
    }
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to validate stitching projection parameters: " + std::string(exception.what()));
  }
  return absl::OkStatus();
}

namespace fs = std::filesystem;

absl::StatusOr<std::vector<fs::directory_entry>> directory_entries(
    const fs::path& directory,
    const std::string& description,
    size_t maximum_entries = 4096) {
  std::error_code error;
  fs::directory_iterator iterator(directory, error);
  if (error)
    return absl::InternalError("Unable to inspect " + description + ": " + error.message());
  std::vector<fs::directory_entry> entries;
  const fs::directory_iterator end;
  while (iterator != end) {
    if (entries.size() >= maximum_entries)
      return absl::ResourceExhaustedError("Too many entries while inspecting " + description);
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

absl::Status link_clone_or_copy_rollback_file(const PinnedRinkRollbackArtifact& source, const fs::path& destination) {
  const char* force_portable_fallback = std::getenv("HM_TEST_RINK_DISABLE_LINK_CLONE");
  return snapshot_rink_artifact_for_rollback(
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
  return publish_transaction_state(transaction, "ROLLED_BACK\n");
}

bool is_rink_artifact_name(const std::string& name) {
  static const std::regex mask_pattern(R"(^rink_mask_(0|[1-9][0-9]*)[.]png$)");
  return name == "config.yaml" || name == "s.png" || std::regex_match(name, mask_pattern);
}

bool is_rink_mask_name(const std::string& name) {
  static const std::regex mask_pattern(R"(^rink_mask_(0|[1-9][0-9]*)[.]png$)");
  return std::regex_match(name, mask_pattern);
}

absl::StatusOr<std::unique_ptr<ScopedRinkLock>> lock_rink_transactions(const fs::path& root, bool wait) {
  const fs::path path = root / ".hstream-rink.lock";
  const int descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (descriptor < 0)
    return absl::InternalError("Unable to open rink transaction lock: " + std::string(std::strerror(errno)));
  if (::flock(descriptor, LOCK_EX | (wait ? 0 : LOCK_NB)) != 0) {
    const int lock_errno = errno;
    const std::string message = std::strerror(lock_errno);
    ::close(descriptor);
    if (!wait && (lock_errno == EWOULDBLOCK || lock_errno == EAGAIN))
      return absl::UnavailableError("Rink transaction is being updated");
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
  struct stat metadata {};
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
    if (names.size() > kMaximumRinkTransactionArtifacts)
      return absl::ResourceExhaustedError("Rink transaction manifest contains too many artifacts");
  }
  if (!input.eof() || !names.count("config.yaml") || names.size() < 2)
    return absl::FailedPreconditionError("Prepared rink transaction manifest is incomplete");
  return names;
}

absl::Status recover_rink_transactions_locked(const fs::path& root) {
  auto scan_required = transaction_recovery_scan_required(root, TransactionJournalKind::kRink);
  if (!scan_required.ok())
    return scan_required.status();
  if (!*scan_required)
    return absl::OkStatus();
  std::error_code error;
  auto opened_root = PinnedDirectory::Open(root, "rink transaction root");
  if (!opened_root.ok())
    return opened_root.status();
  PinnedDirectory root_directory = std::move(*opened_root);
  auto root_entries = directory_entries(root_directory.path(), "rink transactions");
  if (!root_entries.ok())
    return root_entries.status();
  bool recovered = false;
  for (const auto& entry : *root_entries) {
    const std::string directory_name = entry.path().filename().string();
    static const std::regex current_transaction_pattern(R"(^hstream-rink-[A-Za-z0-9]{6}$)");
    const bool current_transaction = std::regex_match(directory_name, current_transaction_pattern);
    const bool legacy_transaction = directory_name.rfind(".hstream-rink-", 0) == 0 &&
        directory_name != ".hstream-rink-journal-v1" && directory_name != ".hstream-rink-recovery-pending";
    if (!current_transaction && !legacy_transaction)
      continue;
    if (current_transaction) {
      std::error_code type_error;
      const fs::file_type type = entry.symlink_status(type_error).type();
      if (type_error == std::errc::no_such_file_or_directory)
        continue;
      if (type_error)
        return absl::InternalError("Unable to inspect visible rink transaction collision: " + type_error.message());
      if (type != fs::file_type::directory)
        continue;
    }
    auto opened_transaction = root_directory.OpenChild(directory_name, "rink transaction directory");
    if (!opened_transaction.ok())
      return opened_transaction.status();
    if (!opened_transaction->has_value())
      continue;
    PinnedDirectory transaction_directory = std::move(**opened_transaction);
    if (current_transaction) {
      auto owned = owned_directory_marker_matches(
          transaction_directory.path(), kOwnedDirectoryMarkerName, kOwnedDirectoryMarkerContents);
      if (!owned.ok())
        return owned.status();
      if (!*owned)
        continue;
    }
    const fs::path transaction = transaction_directory.path();
    auto state = read_rink_transaction_state(transaction);
    if (!state.ok())
      return state.status();
    if (*state == "PREPARED") {
      auto manifest = read_rink_manifest(transaction);
      if (!manifest.ok())
        return manifest.status();
      std::vector<std::string> backup_artifact_names;
      std::set<std::string> backup_names;
      std::optional<PinnedDirectory> previous_directory;
      auto opened_previous = transaction_directory.OpenChild("previous", "rink transaction backup directory");
      if (!opened_previous.ok())
        return opened_previous.status();
      if (!opened_previous->has_value())
        return absl::FailedPreconditionError("Prepared rink transaction has no backup directory");
      previous_directory.emplace(std::move(**opened_previous));
      const fs::path previous = previous_directory->path();
      auto previous_entries = directory_entries(previous, "rink transaction backup", kMaximumRinkTransactionArtifacts);
      if (!previous_entries.ok())
        return previous_entries.status();
      for (const auto& old : *previous_entries) {
        const std::string old_name = old.path().filename().string();
        if (!is_rink_artifact_name(old_name) || manifest->count(old_name) == 0 ||
            !backup_names.insert(old_name).second) {
          return absl::InvalidArgumentError("Invalid or unmanifested rink transaction backup: " + old_name);
        }
        backup_artifact_names.push_back(old_name);
      }
      auto pinned_backups = pin_rink_rollback_artifacts(*previous_directory, backup_artifact_names);
      if (!pinned_backups.ok())
        return pinned_backups.status();
      if (const char* marker = std::getenv("HM_TEST_RINK_RECOVERY_POST_PIN_MARKER"))
        std::ofstream(marker, std::ios::out | std::ios::trunc) << "pinned\n";
      if (const char* delay = std::getenv("HM_TEST_RINK_RECOVERY_POST_PIN_DELAY_MS")) {
        const uint64_t delay_ms = std::strtoull(delay, nullptr, 10);
        if (delay_ms > 0)
          std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint64_t>(delay_ms, 10'000)));
      }

      std::vector<std::pair<fs::path, fs::path>> staged_restores;
      for (const PinnedRinkRollbackArtifact& old : *pinned_backups) {
        const fs::path staged = transaction / (".restore-" + old.name());
        fs::remove(staged, error);
        if (error)
          return absl::InternalError("Unable to remove stale rink restore staging file: " + error.message());
        auto restore = link_clone_or_copy_rollback_file(old, staged);
        if (!restore.ok())
          return restore;
        auto status = fsync_path(staged);
        if (!status.ok())
          return status;
        staged_restores.emplace_back(staged, root_directory.path() / old.name());
      }
      auto status = fsync_path(transaction, true);
      if (!status.ok())
        return status;
      for (const std::string& name : *manifest) {
        if (backup_names.count(name) != 0)
          continue;
        fs::remove(root_directory.path() / name, error);
        if (error)
          return absl::InternalError("Unable to remove interrupted rink artifact: " + error.message());
      }
      size_t restored = 0;
      for (const auto& [staged, destination] : staged_restores) {
        fs::rename(staged, destination, error);
        if (error)
          return absl::InternalError("Unable to atomically restore interrupted rink artifact: " + error.message());
        ++restored;
        if (const char* fail_after = std::getenv("HM_TEST_RINK_ROLLBACK_FAIL_AFTER");
            fail_after != nullptr && restored == static_cast<size_t>(std::strtoull(fail_after, nullptr, 10))) {
          return absl::InternalError("Injected rink rollback interruption");
        }
      }
      error.clear();
      status = fsync_path(root_directory.path(), true);
      if (!status.ok())
        return status;
      status = mark_rink_transaction_rolled_back(transaction);
      if (!status.ok())
        return status;
    }
    auto cleanup = remove_pinned_directory(
        root_directory, directory_name, transaction_directory, current_transaction ? kOwnedDirectoryMarkerName : "");
    if (!cleanup.ok())
      return cleanup;
    recovered = true;
  }
  if (recovered) {
    auto status = fsync_path(root_directory.path(), true);
    if (!status.ok())
      return status;
  }
  return complete_transaction_recovery(root, TransactionJournalKind::kRink);
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

absl::StatusOr<std::vector<double>> read_stitch_projection_parameters(
    const YAML::Node& config,
    StitchProjection projection) {
  try {
    const YAML::Node stitching = config && config.IsMap() ? config["stitching"] : YAML::Node();
    if (stitching && !stitching.IsNull() && !stitching.IsMap())
      return absl::InvalidArgumentError("stitching must be a map");
    const YAML::Node parameters = stitching && stitching.IsMap() ? stitching["projection_parameters"] : YAML::Node();
    const absl::Status validation = validate_projection_parameter_map(parameters);
    if (!validation.ok())
      return validation;
    if (StitchProjectionParameters(projection).empty())
      return std::vector<double>{};
    if (!parameters || !parameters.IsDefined() || parameters.IsNull())
      return DefaultStitchProjectionParameters(projection);
    return parse_projection_parameter_sequence(parameters[StitchProjectionName(projection)], projection);
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to read stitching projection parameters: " + std::string(exception.what()));
  }
}

absl::StatusOr<std::vector<StitchCameraConfiguration>> read_stitch_camera_configurations(const YAML::Node& config) {
  static const std::regex valid_id("^[a-z0-9]+(?:-[a-z0-9]+)*$");
  const std::vector<StitchCameraConfiguration> legacy_default = {
      {.id = "gopro-mission-1", .display_name = "GoPro Mission 1", .horizontal_fov = 127.2, .vertical_fov = 95.0},
      {.id = "gopro-hero-11", .display_name = "GoPro Hero 11", .horizontal_fov = 108.0, .vertical_fov = 90.0},
      {.id = "insta-ace-pro-2", .display_name = "Insta Ace Pro 2", .horizontal_fov = 108.0, .vertical_fov = 90.0}};
  try {
    const YAML::Node stitching = config && config.IsMap() ? config["stitching"] : YAML::Node();
    if (stitching && !stitching.IsNull() && !stitching.IsMap())
      return absl::InvalidArgumentError("stitching must be a map");
    const YAML::Node configurations = stitching && stitching.IsMap() ? stitching["camera_configs"] : YAML::Node();
    if (!configurations || !configurations.IsDefined() || configurations.IsNull())
      return legacy_default;
    if (!configurations.IsMap())
      return absl::InvalidArgumentError("stitching.camera_configs must be a map");

    std::vector<StitchCameraConfiguration> result;
    std::set<std::string> ids;
    for (const auto& entry : configurations) {
      if (!entry.first.IsScalar())
        return absl::InvalidArgumentError("stitching.camera_configs keys must be scalar identifiers");
      const std::string id = entry.first.as<std::string>();
      if (!std::regex_match(id, valid_id) || !ids.insert(id).second) {
        return absl::InvalidArgumentError(
            "stitching.camera_configs identifiers must be unique lowercase kebab-case values");
      }
      if (!entry.second.IsMap())
        return absl::InvalidArgumentError("stitching.camera_configs." + id + " must be a map");
      const YAML::Node display_name = entry.second["display_name"];
      const YAML::Node horizontal_fov = entry.second["horizontal_fov"];
      const YAML::Node vertical_fov = entry.second["vertical_fov"];
      if (!display_name || !display_name.IsScalar() || display_name.as<std::string>().empty()) {
        return absl::InvalidArgumentError("stitching.camera_configs." + id + ".display_name must be a nonempty scalar");
      }
      if (!horizontal_fov || !horizontal_fov.IsScalar() || !vertical_fov || !vertical_fov.IsScalar()) {
        return absl::InvalidArgumentError(
            "stitching.camera_configs." + id + " must define numeric horizontal_fov and vertical_fov values");
      }
      StitchCameraConfiguration camera{
          .id = id,
          .display_name = display_name.as<std::string>(),
          .horizontal_fov = horizontal_fov.as<double>(),
          .vertical_fov = vertical_fov.as<double>()};
      if (!std::isfinite(camera.horizontal_fov) || camera.horizontal_fov <= 0.0 || camera.horizontal_fov >= 360.0) {
        return absl::InvalidArgumentError(
            "stitching.camera_configs." + id + ".horizontal_fov must be finite and between 0 and 360 degrees");
      }
      if (!std::isfinite(camera.vertical_fov) || camera.vertical_fov <= 0.0 || camera.vertical_fov > 180.0) {
        return absl::InvalidArgumentError(
            "stitching.camera_configs." + id + ".vertical_fov must be finite and between 0 and 180 degrees");
      }
      result.push_back(std::move(camera));
    }
    if (result.empty())
      return absl::InvalidArgumentError("stitching.camera_configs must contain at least one configuration");
    return result;
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to read stitching camera configurations: " + std::string(exception.what()));
  }
}

absl::StatusOr<StitchCameraSelection> read_stitch_camera_selection(const YAML::Node& config) {
  try {
    std::vector<StitchCameraConfiguration> configurations;
    HM_ASSIGN_OR_RETURN(configurations, read_stitch_camera_configurations(config));
    const YAML::Node stitching = config && config.IsMap() ? config["stitching"] : YAML::Node();
    const YAML::Node configured = stitching && stitching.IsMap() ? stitching["camera_config"] : YAML::Node();
    if (configured && configured.IsDefined() && !configured.IsNull() && !configured.IsScalar())
      return absl::InvalidArgumentError("stitching.camera_config must be a scalar identifier");
    const std::string configuration = configured && configured.IsDefined() && !configured.IsNull()
        ? configured.as<std::string>()
        : configurations[0].id;
    static const std::regex valid_id("^[a-z0-9]+(?:-[a-z0-9]+)*$");
    if (!std::regex_match(configuration, valid_id)) {
      return absl::InvalidArgumentError("stitching.camera_config must be a lowercase kebab-case identifier");
    }
    auto selected = std::find_if(configurations.begin(), configurations.end(), [&](const auto& candidate) {
      return candidate.id == configuration;
    });

    const YAML::Node fov = stitching && stitching.IsMap() ? stitching["camera_fov"] : YAML::Node();
    if (fov && fov.IsDefined() && !fov.IsNull() && !fov.IsMap())
      return absl::InvalidArgumentError("stitching.camera_fov must be a map");
    if (fov && fov.IsMap()) {
      static const std::set<std::string> supported_keys = {"horizontal_fov", "vertical_fov"};
      for (const auto& entry : fov) {
        if (!entry.first.IsScalar() || !supported_keys.count(entry.first.as<std::string>()))
          return absl::InvalidArgumentError("stitching.camera_fov contains an unsupported key");
      }
    }
    const YAML::Node horizontal_override = fov && fov.IsMap() ? fov["horizontal_fov"] : YAML::Node();
    const YAML::Node vertical_override = fov && fov.IsMap() ? fov["vertical_fov"] : YAML::Node();
    const bool has_horizontal = horizontal_override && horizontal_override.IsDefined() && !horizontal_override.IsNull();
    const bool has_vertical = vertical_override && vertical_override.IsDefined() && !vertical_override.IsNull();
    if ((has_horizontal && !horizontal_override.IsScalar()) || (has_vertical && !vertical_override.IsScalar())) {
      return absl::InvalidArgumentError("stitching.camera_fov values must be numeric scalars");
    }
    if (selected == configurations.end() && (!has_horizontal || !has_vertical)) {
      return absl::InvalidArgumentError(
          "Unknown stitching.camera_config \"" + configuration +
          "\"; define it in stitching.camera_configs or "
          "provide both stitching.camera_fov overrides");
    }
    StitchCameraSelection result{
        .configuration = configuration,
        .horizontal_fov = has_horizontal ? horizontal_override.as<double>() : selected->horizontal_fov,
        .vertical_fov = has_vertical ? vertical_override.as<double>() : selected->vertical_fov};
    if (!std::isfinite(result.horizontal_fov) || result.horizontal_fov <= 0.0 || result.horizontal_fov >= 360.0) {
      return absl::InvalidArgumentError(
          "stitching.camera_fov.horizontal_fov must be finite and between 0 and 360 degrees");
    }
    if (!std::isfinite(result.vertical_fov) || result.vertical_fov <= 0.0 || result.vertical_fov > 180.0) {
      return absl::InvalidArgumentError(
          "stitching.camera_fov.vertical_fov must be finite and between 0 and 180 degrees");
    }
    return result;
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError("Unable to read stitching camera selection: " + std::string(exception.what()));
  }
}

void write_stitch_camera_selection(YAML::Node& config, const StitchCameraSelection& selection) {
  config["stitching"]["camera_config"] = selection.configuration;
  config["stitching"]["camera_fov"]["horizontal_fov"] = selection.horizontal_fov;
  config["stitching"]["camera_fov"]["vertical_fov"] = selection.vertical_fov;
}

void write_stitch_projection_parameters(
    YAML::Node& config,
    StitchProjection projection,
    const std::vector<double>& parameters) {
  if (StitchProjectionParameters(projection).empty())
    return;
  YAML::Node values(YAML::NodeType::Sequence);
  for (double parameter : parameters)
    values.push_back(parameter);
  config["stitching"]["projection_parameters"][StitchProjectionName(projection)] = values;
}

absl::StatusOr<std::vector<StitchRinkConfiguration>> read_stitch_rink_configurations(const YAML::Node& config) {
  try {
    const YAML::Node stitching = config && config.IsMap() ? config["stitching"] : YAML::Node();
    if (stitching && !stitching.IsNull() && !stitching.IsMap())
      return absl::InvalidArgumentError("stitching must be a map");
    const YAML::Node configured_definitions = stitching && stitching.IsMap() ? stitching["rink_configs"] : YAML::Node();
    YAML::Node definitions;
    if (configured_definitions && !configured_definitions.IsNull()) {
      definitions.reset(configured_definitions);
    } else {
      const auto baseline = hm::baseline_config::load();
      if (!baseline.ok())
        return baseline.status();
      const YAML::Node baseline_values = baseline->values;
      const YAML::Node baseline_stitching = baseline_values["stitching"];
      definitions.reset(
          baseline_stitching && baseline_stitching.IsMap() ? baseline_stitching["rink_configs"] : YAML::Node());
      if (!definitions || definitions.IsNull())
        return std::vector<StitchRinkConfiguration>{};
    }
    if (!definitions.IsMap())
      return absl::InvalidArgumentError("stitching.rink_configs must be a map");
    static const std::regex valid_id("^[a-z0-9]+(?:-[a-z0-9]+)*$");
    std::set<std::string> ids;
    std::vector<StitchRinkConfiguration> result;
    for (const auto& entry : definitions) {
      if (!entry.first.IsScalar() || !entry.second.IsMap())
        return absl::InvalidArgumentError("stitching.rink_configs entries must be named maps");
      const auto id = entry.first.as<std::string>();
      if (!std::regex_match(id, valid_id) || !ids.insert(id).second)
        return absl::InvalidArgumentError("stitching.rink_configs identifiers must be unique lowercase kebab-case");
      for (const auto& field : entry.second) {
        if (!field.first.IsScalar() ||
            (field.first.as<std::string>() != "display_name" && field.first.as<std::string>() != "rotation_degrees"))
          return absl::InvalidArgumentError("Unsupported field in stitching.rink_configs." + id);
      }
      const YAML::Node name = entry.second["display_name"];
      const YAML::Node rotation = entry.second["rotation_degrees"];
      if (!name || !name.IsScalar() || name.as<std::string>().empty() || !rotation || rotation.IsNull())
        return absl::InvalidArgumentError("Rink " + id + " requires display_name and rotation_degrees");
      StitchProjectionFraming view;
      HM_RETURN_IF_ERROR(read_framing_array(entry.second, "rotation_degrees", view.rotation_degrees));
      HM_RETURN_IF_ERROR(validate_projection_view(view));
      result.push_back({id, name.as<std::string>(), view.rotation_degrees});
    }
    return result;
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError("Unable to read stitching rink profiles: " + std::string(exception.what()));
  }
}

absl::StatusOr<std::string> read_stitch_rink_selection(const YAML::Node& config) {
  try {
    const YAML::Node stitching = config && config.IsMap() ? config["stitching"] : YAML::Node();
    if (stitching && !stitching.IsNull() && !stitching.IsMap())
      return absl::InvalidArgumentError("stitching must be a map");
    const YAML::Node selected = stitching && stitching.IsMap() ? stitching["rink_config"] : YAML::Node();
    if (!selected || selected.IsNull())
      return std::string();
    if (!selected.IsScalar())
      return absl::InvalidArgumentError("stitching.rink_config must be a rink identifier");
    const auto id = selected.as<std::string>();
    if (id.empty())
      return id;
    const auto profiles = read_stitch_rink_configurations(config);
    if (!profiles.ok())
      return profiles.status();
    for (const auto& profile : *profiles) {
      if (profile.id == id)
        return id;
    }
    return absl::InvalidArgumentError("Unknown stitching.rink_config \"" + id + "\"");
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError("Unable to read stitching rink selection: " + std::string(exception.what()));
  }
}

bool restore_generated_stitch_rink_context(YAML::Node& config) {
  try {
    const YAML::Node values = config;
    const YAML::Node ui = values && values.IsMap() ? values["hstream_ui"] : YAML::Node();
    const YAML::Node marker = ui && ui.IsMap() ? ui["generated_stitching_rink_context"] : YAML::Node();
    if (!marker || !marker.IsMap() || marker.size() != 2)
      return false;
    const YAML::Node generated = marker["generated"];
    const YAML::Node previous = marker["previous"];
    if (!generated || !generated.IsMap() || !previous || !previous.IsMap() || generated.size() != 2)
      return false;
    const YAML::Node stitching = values["stitching"];
    for (const auto& entry : previous) {
      if (!entry.first.IsScalar() ||
          (entry.first.as<std::string>() != "rink_config" && entry.first.as<std::string>() != "rink_configs"))
        return false;
    }
    if (!generated["rink_config"] || !generated["rink_configs"])
      return false;
    // Preserve edits independently: changing only the selected rink must not
    // turn the untouched generated profile catalog into private defaults.
    for (const char* key : {"rink_config", "rink_configs"}) {
      const YAML::Node current = stitching && stitching.IsMap() ? stitching[key] : YAML::Node();
      if (!current || YAML::Dump(current) != YAML::Dump(generated[key]))
        continue;
      if (previous[key])
        config["stitching"][key] = YAML::Clone(previous[key]);
      else
        config["stitching"].remove(key);
    }
    config["hstream_ui"].remove("generated_stitching_rink_context");
    return true;
  } catch (const YAML::Exception&) {
    return false;
  }
}

absl::StatusOr<bool> materialize_stitch_rink_context(YAML::Node& config, const YAML::Node& effective) {
  try {
    const std::string before = YAML::Dump(config);
    restore_generated_stitch_rink_context(config);
    std::string selected;
    HM_ASSIGN_OR_RETURN(selected, read_stitch_rink_selection(effective));
    const auto current_selection = read_stitch_rink_selection(config);
    bool matching = current_selection.ok() && *current_selection == selected;
    std::vector<StitchRinkConfiguration> profiles;
    if (!selected.empty()) {
      HM_ASSIGN_OR_RETURN(profiles, read_stitch_rink_configurations(effective));
      const auto current_profiles = read_stitch_rink_configurations(config);
      const auto same_selected_profile = [&](const StitchRinkConfiguration& profile) {
        if (profile.id != selected || !current_profiles.ok())
          return false;
        return std::any_of(current_profiles->begin(), current_profiles->end(), [&](const auto& current) {
          return current.id == selected && current.rotation_degrees == profile.rotation_degrees;
        });
      };
      matching = matching && std::any_of(profiles.begin(), profiles.end(), same_selected_profile);
    }
    if (matching)
      return YAML::Dump(config) != before;

    YAML::Node marker(YAML::NodeType::Map);
    marker["previous"] = YAML::Node(YAML::NodeType::Map);
    const YAML::Node values = config;
    const YAML::Node stitching = values && values.IsMap() ? values["stitching"] : YAML::Node();
    for (const char* key : {"rink_config", "rink_configs"}) {
      const YAML::Node value = stitching && stitching.IsMap() ? stitching[key] : YAML::Node();
      if (value)
        marker["previous"][key] = YAML::Clone(value);
    }
    YAML::Node definitions(YAML::NodeType::Map);
    for (const auto& profile : profiles) {
      definitions[profile.id]["display_name"] = profile.display_name;
      definitions[profile.id]["rotation_degrees"] =
          std::vector<double>(profile.rotation_degrees.begin(), profile.rotation_degrees.end());
    }
    config["stitching"]["rink_config"] = selected;
    config["stitching"]["rink_configs"] = definitions;
    marker["generated"]["rink_config"] = selected;
    marker["generated"]["rink_configs"] = YAML::Clone(definitions);
    config["hstream_ui"]["generated_stitching_rink_context"] = marker;
    return YAML::Dump(config) != before;
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError("Unable to materialize stitching rink context: " + std::string(exception.what()));
  }
}

absl::StatusOr<StitchProjectionFraming> read_stitch_projection_framing(const YAML::Node& config) {
  StitchProjectionFraming result;
  result.rotation_inherited = true;
  try {
    std::string rink;
    HM_ASSIGN_OR_RETURN(rink, read_stitch_rink_selection(config));
    if (!rink.empty()) {
      std::vector<StitchRinkConfiguration> profiles;
      HM_ASSIGN_OR_RETURN(profiles, read_stitch_rink_configurations(config));
      for (const auto& profile : profiles) {
        if (profile.id == rink)
          result.rotation_degrees = profile.rotation_degrees;
      }
    }
    const YAML::Node stitching = config && config.IsMap() ? config["stitching"] : YAML::Node();
    if (stitching && !stitching.IsNull() && !stitching.IsMap())
      return absl::InvalidArgumentError("stitching must be a map");
    const YAML::Node framing = stitching && stitching.IsMap() ? stitching["projection_framing"] : YAML::Node();
    if (!framing || !framing.IsDefined() || framing.IsNull())
      return result;
    if (!framing.IsMap())
      return absl::InvalidArgumentError("stitching.projection_framing must be a map");
    static const std::set<std::string> supported_keys = {
        "auto_fov", "horizontal_fov", "auto_canvas", "auto_crop", "rotation_degrees", "crop"};
    for (const auto& entry : framing) {
      if (!entry.first.IsScalar())
        return absl::InvalidArgumentError("stitching.projection_framing keys must be scalar values");
      const std::string key = entry.first.as<std::string>();
      if (!supported_keys.count(key)) {
        return absl::InvalidArgumentError("Unsupported stitching.projection_framing key \"" + key + "\"");
      }
    }
    auto read_bool = [&](const char* key, bool fallback) -> absl::StatusOr<bool> {
      const YAML::Node value = framing[key];
      if (!value || !value.IsDefined() || value.IsNull())
        return fallback;
      if (!value.IsScalar())
        return absl::InvalidArgumentError(std::string("stitching.projection_framing.") + key + " must be boolean");
      try {
        return value.as<bool>();
      } catch (const YAML::Exception& exception) {
        return absl::InvalidArgumentError(
            std::string("stitching.projection_framing.") + key + " must be true or false: " + exception.what());
      }
    };
    auto auto_fov = read_bool("auto_fov", result.auto_fov);
    if (!auto_fov.ok())
      return auto_fov.status();
    result.auto_fov = *auto_fov;
    auto auto_canvas = read_bool("auto_canvas", result.auto_canvas);
    if (!auto_canvas.ok())
      return auto_canvas.status();
    result.auto_canvas = *auto_canvas;
    auto auto_crop = read_bool("auto_crop", result.auto_crop);
    if (!auto_crop.ok())
      return auto_crop.status();
    result.auto_crop = *auto_crop;
    const YAML::Node horizontal_fov = framing["horizontal_fov"];
    if (horizontal_fov && horizontal_fov.IsDefined() && !horizontal_fov.IsNull()) {
      if (!horizontal_fov.IsScalar()) {
        return absl::InvalidArgumentError("stitching.projection_framing.horizontal_fov must be a numeric scalar");
      }
      result.horizontal_fov = horizontal_fov.as<double>();
    }
    if (!std::isfinite(result.horizontal_fov) || result.horizontal_fov <= 0.0 || result.horizontal_fov > 360.0) {
      return absl::InvalidArgumentError(
          "stitching.projection_framing.horizontal_fov must be finite and between 0 and 360 degrees");
    }
    const YAML::Node rotation = framing["rotation_degrees"];
    result.rotation_inherited = !rotation || rotation.IsNull();
    HM_RETURN_IF_ERROR(read_framing_array(framing, "rotation_degrees", result.rotation_degrees));
    HM_RETURN_IF_ERROR(read_framing_array(framing, "crop", result.crop));
    HM_RETURN_IF_ERROR(validate_projection_view(result));
    return result;
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError("Unable to read stitching projection framing: " + std::string(exception.what()));
  }
}

void write_stitch_projection_framing(YAML::Node& config, const StitchProjectionFraming& framing) {
  YAML::Node node = config["stitching"]["projection_framing"];
  node["auto_fov"] = framing.auto_fov;
  node["horizontal_fov"] = framing.horizontal_fov;
  node["auto_canvas"] = framing.auto_canvas;
  node["auto_crop"] = framing.auto_crop;
  write_projection_view(node, framing);
  if (framing.rotation_inherited)
    node.remove("rotation_degrees");
}

absl::Status ValidateStitchProjectionFraming(
    StitchProjection projection,
    const std::vector<double>& projection_parameters,
    const StitchProjectionFraming& framing) {
  HM_RETURN_IF_ERROR(validate_projection_view(framing));
  const absl::Status parameter_status = ValidateStitchProjectionParameters(projection, projection_parameters);
  if (!parameter_status.ok())
    return parameter_status;
  if (!std::isfinite(framing.horizontal_fov) || framing.horizontal_fov <= 0.0 || framing.horizontal_fov > 360.0) {
    return absl::InvalidArgumentError(
        "stitching.projection_framing.horizontal_fov must be finite and between 0 and 360 degrees");
  }
  if (!framing.auto_fov) {
    const absl::Status fov_status =
        ValidateStitchProjectionHorizontalFov(projection, projection_parameters, framing.horizontal_fov);
    if (!fov_status.ok()) {
      return absl::InvalidArgumentError(
          "Invalid stitching.projection_framing.horizontal_fov: " + std::string(fov_status.message()));
    }
  }
  return absl::OkStatus();
}

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
  auto rink = lock_rink_transactions(game_dir, /*wait=*/true);
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

absl::StatusOr<std::unique_ptr<GameConfigTransactionLock>> GameConfigTransactionLock::TryAcquire(
    const fs::path& game_dir) {
  auto config = GameConfigLock::TryAcquire(game_dir);
  if (!config.ok())
    return config.status();
  auto rink = lock_rink_transactions(game_dir, /*wait=*/false);
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
  if (std::getenv("HSTREAM_GAME_CONFIG_TEST_INTERRUPT_BEFORE_RENAME") != nullptr) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return absl::InternalError("Injected interruption before game config publication");
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
  if (invalidated_artifacts.size() >= kMaximumRinkTransactionArtifacts)
    return absl::ResourceExhaustedError("Rink invalidation contains too many transaction artifacts");

  std::vector<fs::path> old_files = invalidated_artifacts;
  const fs::path current_config = game_dir / "config.yaml";
  if (fs::exists(current_config, error)) {
    if (error || !fs::is_regular_file(current_config, error) || error)
      return absl::FailedPreconditionError("Game config is not a regular file: " + current_config.string());
    old_files.push_back(current_config);
  } else if (error) {
    return absl::InternalError("Unable to inspect game config: " + error.message());
  }
  auto opened_root = PinnedDirectory::Open(game_dir, "rink invalidation root");
  if (!opened_root.ok())
    return opened_root.status();
  std::vector<std::string> old_names;
  old_names.reserve(old_files.size());
  for (const fs::path& old : old_files)
    old_names.push_back(old.filename().string());
  auto pinned_old_files = pin_rink_rollback_artifacts(*opened_root, old_names);
  if (!pinned_old_files.ok())
    return pinned_old_files.status();

  auto pending_status = mark_transaction_recovery_pending(game_dir, TransactionJournalKind::kRink);
  if (!pending_status.ok())
    return pending_status;
  std::string pattern = (game_dir / "hstream-rink-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  char* created = ::mkdtemp(writable.data());
  if (created == nullptr)
    return absl::InternalError("Unable to create rink invalidation staging directory");
  const fs::path staging(created);
  struct Cleanup {
    fs::path path;
    bool owned{false};
    bool prepared{false};
    ~Cleanup() {
      if (prepared)
        return;
      if (owned) {
        (void)remove_owned_directory(path, kOwnedDirectoryMarkerName, kOwnedDirectoryMarkerContents);
      } else {
        std::error_code ignored;
        fs::remove(path, ignored);
      }
    }
  } cleanup{staging};
  if (::chmod(staging.c_str(), 0700) != 0)
    return absl::InternalError("Unable to protect rink invalidation staging directory");
  auto marker_status = write_owned_directory_marker(staging, kOwnedDirectoryMarkerName, kOwnedDirectoryMarkerContents);
  if (!marker_status.ok())
    return marker_status;
  cleanup.owned = true;

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

  for (const PinnedRinkRollbackArtifact& old : *pinned_old_files) {
    auto preserve = link_clone_or_copy_rollback_file(old, previous / old.name());
    if (!preserve.ok())
      return preserve;
    status = fsync_path(previous / old.name());
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
  status = remove_owned_directory(staging, kOwnedDirectoryMarkerName, kOwnedDirectoryMarkerContents);
  if (!status.ok())
    return status;
  status = fsync_path(game_dir, true);
  if (!status.ok())
    return status;
  status = complete_transaction_recovery(game_dir, TransactionJournalKind::kRink);
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

namespace {

void write_projection_framing_node(YAML::Node node, const StitchProjectionFraming& framing) {
  node["auto_fov"] = framing.auto_fov;
  node["horizontal_fov"] = framing.horizontal_fov;
  node["auto_canvas"] = framing.auto_canvas;
  node["auto_crop"] = framing.auto_crop;
  write_projection_view(node, framing);
}

absl::StatusOr<StitchProjectionFraming> read_projection_framing_node(const YAML::Node& node) {
  YAML::Node wrapper(YAML::NodeType::Map);
  if (node && node.IsDefined())
    wrapper["stitching"]["projection_framing"] = YAML::Clone(node);
  return read_stitch_projection_framing(wrapper);
}

void write_camera_selection_node(YAML::Node node, const StitchCameraSelection& camera) {
  node["camera_config"] = camera.configuration;
  node["camera_fov"]["horizontal_fov"] = camera.horizontal_fov;
  node["camera_fov"]["vertical_fov"] = camera.vertical_fov;
}

absl::StatusOr<StitchCameraSelection> read_camera_selection_node(const YAML::Node& node) {
  YAML::Node wrapper(YAML::NodeType::Map);
  if (node && node.IsMap()) {
    if (node["camera_config"])
      wrapper["stitching"]["camera_config"] = YAML::Clone(node["camera_config"]);
    if (node["camera_fov"])
      wrapper["stitching"]["camera_fov"] = YAML::Clone(node["camera_fov"]);
  }
  return read_stitch_camera_selection(wrapper);
}

absl::StatusOr<StitchCameraSelection> read_worker_camera_selection(const YAML::Node& config) {
  const auto baseline = hm::baseline_config::load();
  if (!baseline.ok())
    return baseline.status();
  YAML::Node effective = YAML::Clone(baseline->values);
  const YAML::Node stitching = config && config.IsMap() ? config["stitching"] : YAML::Node();
  if (stitching && !stitching.IsNull() && !stitching.IsMap())
    return absl::InvalidArgumentError("stitching must be a map");
  for (const char* key : {"camera_configs", "camera_config", "camera_fov"}) {
    const YAML::Node value = stitching && stitching.IsMap() ? stitching[key] : YAML::Node();
    if (value && value.IsDefined())
      effective["stitching"][key] = YAML::Clone(value);
  }
  return read_stitch_camera_selection(effective);
}

absl::Status validate_backend_generation_claim(
    const YAML::Node& config,
    const std::string& expected_invalidation_id,
    const StitchingBackendChoices& expected_choices,
    bool validate_worker_tuple) {
  const absl::Status generation_status = validate_stitching_generation_owner(config, expected_invalidation_id);
  if (!generation_status.ok())
    return generation_status;
  try {
    const YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
    const YAML::Node claim = calibration["backend_generation"];
    if (!claim || !claim.IsMap() || !claim["invalidation_id"] || !claim["invalidation_id"].IsScalar() ||
        !claim["control_point_matcher"] || !claim["control_point_matcher"].IsScalar() || !claim["mapping_backend"] ||
        !claim["mapping_backend"].IsScalar() || !claim["projection"] || !claim["projection"].IsScalar() ||
        !claim["run_autooptimizer"] || !claim["run_autooptimizer"].IsScalar() || !claim["projection_parameters"] ||
        !claim["projection_parameters"].IsSequence() || !claim["projection_framing"] ||
        !claim["projection_framing"].IsMap() || !claim["camera_config"] || !claim["camera_config"].IsScalar() ||
        !claim["camera_fov"] || !claim["camera_fov"].IsMap()) {
      return absl::AbortedError("Stitching backend generation claim is missing or incomplete");
    }
    auto parsed_expected_projection = ParseStitchProjection(expected_choices.projection);
    if (!parsed_expected_projection.ok())
      return parsed_expected_projection.status();
    const StitchProjection expected_projection = *parsed_expected_projection;
    auto parsed_expected_backend = ParseMappingBackend(expected_choices.mapping_backend);
    if (!parsed_expected_backend.ok())
      return parsed_expected_backend.status();
    auto parsed_claim_parameters =
        parse_projection_parameter_sequence(claim["projection_parameters"], expected_projection);
    if (!parsed_claim_parameters.ok())
      return parsed_claim_parameters.status();
    const std::vector<double>& claim_parameters = *parsed_claim_parameters;
    auto parsed_claim_framing = read_projection_framing_node(claim["projection_framing"]);
    if (!parsed_claim_framing.ok())
      return parsed_claim_framing.status();
    auto parsed_claim_camera = read_camera_selection_node(claim);
    if (!parsed_claim_camera.ok())
      return parsed_claim_camera.status();
    if (*parsed_expected_backend == MappingBackend::kNona) {
      const absl::Status expected_framing_status = ValidateStitchProjectionFraming(
          expected_projection, expected_choices.projection_parameters, expected_choices.projection_framing);
      if (!expected_framing_status.ok())
        return expected_framing_status;
      const absl::Status claim_framing_status =
          ValidateStitchProjectionFraming(expected_projection, claim_parameters, *parsed_claim_framing);
      if (!claim_framing_status.ok())
        return claim_framing_status;
    }
    const bool claim_framing_matches = *parsed_expected_backend != MappingBackend::kNona ||
        *parsed_claim_framing == expected_choices.projection_framing;
    const bool claim_matches = claim["invalidation_id"].as<std::string>() == expected_invalidation_id &&
        claim["control_point_matcher"].as<std::string>() == expected_choices.control_point_matcher &&
        claim["mapping_backend"].as<std::string>() == expected_choices.mapping_backend &&
        claim["projection"].as<std::string>() == expected_choices.projection &&
        claim["run_autooptimizer"].as<bool>() == expected_choices.run_autooptimizer &&
        claim_parameters == expected_choices.projection_parameters && claim_framing_matches &&
        *parsed_claim_camera == expected_choices.camera;
    if (!claim_matches) {
      auto format_parameters = [](const std::vector<double>& parameters) {
        std::ostringstream output;
        output << '[';
        for (size_t index = 0; index < parameters.size(); ++index) {
          if (index != 0)
            output << ',';
          output << parameters[index];
        }
        output << ']';
        return output.str();
      };
      std::ostringstream detail;
      detail << "Stitching backend choices were superseded for this calibration generation: expected {id="
             << expected_invalidation_id << ", matcher=" << expected_choices.control_point_matcher
             << ", backend=" << expected_choices.mapping_backend << ", projection=" << expected_choices.projection
             << ", autooptimizer=" << (expected_choices.run_autooptimizer ? "true" : "false")
             << ", parameters=" << format_parameters(expected_choices.projection_parameters)
             << "}, reserved {id=" << claim["invalidation_id"].as<std::string>()
             << ", matcher=" << claim["control_point_matcher"].as<std::string>()
             << ", backend=" << claim["mapping_backend"].as<std::string>()
             << ", projection=" << claim["projection"].as<std::string>()
             << ", autooptimizer=" << (claim["run_autooptimizer"].as<bool>() ? "true" : "false")
             << ", parameters=" << format_parameters(claim_parameters)
             << ", framing={auto-fov=" << (parsed_claim_framing->auto_fov ? "true" : "false")
             << ", fov=" << parsed_claim_framing->horizontal_fov
             << ", auto-canvas=" << (parsed_claim_framing->auto_canvas ? "true" : "false")
             << ", auto-crop=" << (parsed_claim_framing->auto_crop ? "true" : "false") << "}}";
      detail << ", camera={config=" << parsed_claim_camera->configuration
             << ", hfov=" << parsed_claim_camera->horizontal_fov << ", vfov=" << parsed_claim_camera->vertical_fov
             << "}";
      return absl::AbortedError(detail.str());
    }
    if (!validate_worker_tuple)
      return absl::OkStatus();

    const YAML::Node stitching = config["stitching"];
    if (!stitching || !stitching.IsMap() || !stitching["control_point_matcher"] ||
        !stitching["control_point_matcher"].IsScalar() || !stitching["mapping_backend"] ||
        !stitching["mapping_backend"].IsScalar() || !stitching["projection"] || !stitching["projection"].IsScalar() ||
        !stitching["run_autooptimizer"] || !stitching["run_autooptimizer"].IsScalar()) {
      return absl::AbortedError("Worker-visible stitching backend choices are missing or incomplete");
    }
    auto parsed_worker_parameters = read_stitch_projection_parameters(config, expected_projection);
    if (!parsed_worker_parameters.ok())
      return parsed_worker_parameters.status();
    const std::vector<double>& worker_parameters = *parsed_worker_parameters;
    auto worker_framing = read_stitch_projection_framing(config);
    if (!worker_framing.ok())
      return worker_framing.status();
    if (*parsed_expected_backend == MappingBackend::kNona) {
      const absl::Status worker_framing_status =
          ValidateStitchProjectionFraming(expected_projection, worker_parameters, *worker_framing);
      if (!worker_framing_status.ok())
        return worker_framing_status;
    }
    const bool worker_framing_matches =
        *parsed_expected_backend != MappingBackend::kNona || *worker_framing == expected_choices.projection_framing;
    auto worker_camera = read_worker_camera_selection(config);
    if (!worker_camera.ok())
      return worker_camera.status();
    const bool worker_tuple_matches =
        stitching["control_point_matcher"].as<std::string>() == expected_choices.control_point_matcher &&
        stitching["mapping_backend"].as<std::string>() == expected_choices.mapping_backend &&
        stitching["projection"].as<std::string>() == expected_choices.projection &&
        stitching["run_autooptimizer"].as<bool>() == expected_choices.run_autooptimizer &&
        worker_parameters == expected_choices.projection_parameters && worker_framing_matches &&
        *worker_camera == expected_choices.camera;
    if (!worker_tuple_matches)
      return absl::AbortedError("Worker-visible stitching backend choices changed during calibration");
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to validate stitching backend generation: " + std::string(exception.what()));
  }
  return absl::OkStatus();
}

} // namespace

absl::Status reserve_stitching_backend_generation(
    const fs::path& game_dir,
    const std::string& expected_invalidation_id,
    const StitchingBackendChoices& expected_choices) {
  if (expected_invalidation_id.empty())
    return absl::OkStatus();
  auto transaction = GameConfigTransactionLock::Acquire(game_dir);
  if (!transaction.ok())
    return transaction.status();
  const fs::path config_path = game_dir / "config.yaml";
  YAML::Node config;
  try {
    config = fs::is_regular_file(config_path) ? YAML::LoadFile(config_path.string()) : YAML::Node();
    const absl::Status reservation =
        reserve_stitching_backend_generation_in_config(config, expected_invalidation_id, expected_choices);
    if (!reservation.ok())
      return reservation;
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to reserve stitching backend generation: " + std::string(exception.what()));
  }
  return publish_game_config(game_dir, YAML::Dump(config) + "\n");
}

absl::Status reserve_stitching_backend_generation_in_config(
    YAML::Node& config,
    const std::string& expected_invalidation_id,
    const StitchingBackendChoices& expected_choices) {
  if (expected_invalidation_id.empty())
    return absl::OkStatus();
  const absl::Status generation_status = validate_stitching_generation_owner(config, expected_invalidation_id);
  if (!generation_status.ok())
    return generation_status;
  try {
    YAML::Node claim = config["hstream_ui"]["stitching_calibration"]["backend_generation"];
    const bool claim_has_current_generation = claim && claim.IsMap() && claim["invalidation_id"] &&
        claim["invalidation_id"].IsScalar() && claim["invalidation_id"].as<std::string>() == expected_invalidation_id;
    if (!claim_has_current_generation) {
      claim["invalidation_id"] = expected_invalidation_id;
      claim["control_point_matcher"] = expected_choices.control_point_matcher;
      claim["mapping_backend"] = expected_choices.mapping_backend;
      claim["projection"] = expected_choices.projection;
      claim["run_autooptimizer"] = expected_choices.run_autooptimizer;
      YAML::Node projection_parameters(YAML::NodeType::Sequence);
      for (double parameter : expected_choices.projection_parameters)
        projection_parameters.push_back(parameter);
      claim["projection_parameters"] = projection_parameters;
      write_projection_framing_node(claim["projection_framing"], expected_choices.projection_framing);
      write_camera_selection_node(claim, expected_choices.camera);
    } else {
      // Claims created before parameter-aware calibration contain the other
      // immutable choices. Extend only an exactly matching legacy claim;
      // malformed or competing claims must fail validation without mutation.
      const bool legacy_claim = claim["control_point_matcher"] && claim["control_point_matcher"].IsScalar() &&
          claim["mapping_backend"] && claim["mapping_backend"].IsScalar() &&
          (!claim["projection"] || !claim["projection"].IsDefined() || claim["projection"].IsNull() ||
           claim["projection"].IsScalar()) &&
          (!claim["projection_parameters"] || !claim["projection_parameters"].IsDefined() ||
           claim["projection_parameters"].IsNull()) &&
          claim["run_autooptimizer"] && claim["run_autooptimizer"].IsScalar();
      if (legacy_claim) {
        const bool legacy_projection_matches = !claim["projection"] || !claim["projection"].IsDefined() ||
            claim["projection"].IsNull() || claim["projection"].as<std::string>() == expected_choices.projection;
        const bool legacy_matches =
            claim["control_point_matcher"].as<std::string>() == expected_choices.control_point_matcher &&
            claim["mapping_backend"].as<std::string>() == expected_choices.mapping_backend &&
            legacy_projection_matches && claim["run_autooptimizer"].as<bool>() == expected_choices.run_autooptimizer;
        if (!legacy_matches)
          return absl::AbortedError("Stitching backend choices were superseded for this calibration generation");
        YAML::Node projection_parameters(YAML::NodeType::Sequence);
        for (double parameter : expected_choices.projection_parameters)
          projection_parameters.push_back(parameter);
        claim["projection"] = expected_choices.projection;
        claim["projection_parameters"] = projection_parameters;
      }
      const bool framing_missing = !claim["projection_framing"] || !claim["projection_framing"].IsDefined() ||
          claim["projection_framing"].IsNull();
      if (framing_missing) {
        auto parsed_projection = ParseStitchProjection(expected_choices.projection);
        auto parsed_parameters = parsed_projection.ok()
            ? parse_projection_parameter_sequence(claim["projection_parameters"], *parsed_projection)
            : absl::StatusOr<std::vector<double>>(parsed_projection.status());
        const bool current_tuple_matches = parsed_parameters.ok() && claim["control_point_matcher"] &&
            claim["control_point_matcher"].IsScalar() && claim["mapping_backend"] &&
            claim["mapping_backend"].IsScalar() && claim["projection"] && claim["projection"].IsScalar() &&
            claim["run_autooptimizer"] && claim["run_autooptimizer"].IsScalar() &&
            claim["control_point_matcher"].as<std::string>() == expected_choices.control_point_matcher &&
            claim["mapping_backend"].as<std::string>() == expected_choices.mapping_backend &&
            claim["projection"].as<std::string>() == expected_choices.projection &&
            claim["run_autooptimizer"].as<bool>() == expected_choices.run_autooptimizer &&
            *parsed_parameters == expected_choices.projection_parameters;
        if (!current_tuple_matches)
          return absl::AbortedError("Stitching backend choices were superseded for this calibration generation");
        write_projection_framing_node(claim["projection_framing"], expected_choices.projection_framing);
      }
      const bool camera_missing = !claim["camera_config"] || !claim["camera_config"].IsDefined() ||
          claim["camera_config"].IsNull() || !claim["camera_fov"] || !claim["camera_fov"].IsDefined() ||
          claim["camera_fov"].IsNull();
      if (camera_missing) {
        auto parsed_projection = ParseStitchProjection(expected_choices.projection);
        auto parsed_parameters = parsed_projection.ok()
            ? parse_projection_parameter_sequence(claim["projection_parameters"], *parsed_projection)
            : absl::StatusOr<std::vector<double>>(parsed_projection.status());
        const bool current_tuple_matches = parsed_parameters.ok() && claim["control_point_matcher"] &&
            claim["control_point_matcher"].IsScalar() && claim["mapping_backend"] &&
            claim["mapping_backend"].IsScalar() && claim["projection"] && claim["projection"].IsScalar() &&
            claim["run_autooptimizer"] && claim["run_autooptimizer"].IsScalar() &&
            claim["control_point_matcher"].as<std::string>() == expected_choices.control_point_matcher &&
            claim["mapping_backend"].as<std::string>() == expected_choices.mapping_backend &&
            claim["projection"].as<std::string>() == expected_choices.projection &&
            claim["run_autooptimizer"].as<bool>() == expected_choices.run_autooptimizer &&
            *parsed_parameters == expected_choices.projection_parameters;
        if (!current_tuple_matches)
          return absl::AbortedError("Stitching backend choices were superseded for this calibration generation");
        write_camera_selection_node(claim, expected_choices.camera);
      }
    }
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to reserve stitching backend generation: " + std::string(exception.what()));
  }
  return validate_backend_generation_claim(
      config, expected_invalidation_id, expected_choices, /*validate_worker_tuple=*/true);
}

absl::Status validate_stitching_backend_generation(
    const YAML::Node& config,
    const std::string& expected_invalidation_id,
    const StitchingBackendChoices& expected_choices) {
  if (expected_invalidation_id.empty())
    return absl::OkStatus();
  return validate_backend_generation_claim(
      config, expected_invalidation_id, expected_choices, /*validate_worker_tuple=*/true);
}

absl::Status validate_stitching_backend_generation_file_locked(
    const fs::path& config_path,
    const std::string& expected_invalidation_id,
    const StitchingBackendChoices& expected_choices) {
  if (expected_invalidation_id.empty())
    return absl::OkStatus();
  try {
    const YAML::Node config = fs::is_regular_file(config_path) ? YAML::LoadFile(config_path.string()) : YAML::Node();
    return validate_stitching_backend_generation(config, expected_invalidation_id, expected_choices);
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to load stitching backend generation for validation: " + std::string(exception.what()));
  }
}

absl::StatusOr<StitchingBackendChoices> apply_stitching_leveling_rotation(
    const fs::path& game_dir,
    const std::string& expected_invalidation_id,
    const StitchingBackendChoices& expected_choices,
    const std::array<double, 3>& rotation_degrees) {
  auto backend = ParseMappingBackend(expected_choices.mapping_backend);
  if (!backend.ok() || *backend != MappingBackend::kNona)
    return absl::FailedPreconditionError("Interactive rink leveling requires the NONA mapping backend");
  if (expected_invalidation_id.empty())
    return absl::InvalidArgumentError("Interactive rink leveling requires a calibration generation ID");
  for (double angle : rotation_degrees) {
    if (!std::isfinite(angle) || angle < -180.0 || angle > 180.0)
      return absl::InvalidArgumentError("Interactive rink leveling angles must be between -180 and 180 degrees");
  }

  StitchingBackendChoices updated = expected_choices;
  updated.projection_framing.rotation_degrees = rotation_degrees;
  updated.projection_framing.rotation_inherited = false;
  auto transaction = GameConfigTransactionLock::Acquire(game_dir);
  if (!transaction.ok())
    return transaction.status();
  const fs::path config_path = game_dir / "config.yaml";
  try {
    YAML::Node config = fs::is_regular_file(config_path) ? YAML::LoadFile(config_path.string()) : YAML::Node();
    HM_RETURN_IF_ERROR(validate_stitching_backend_generation(config, expected_invalidation_id, expected_choices));
    write_stitch_projection_framing(config, updated.projection_framing);
    write_projection_framing_node(
        config["hstream_ui"]["stitching_calibration"]["backend_generation"]["projection_framing"],
        updated.projection_framing);
    HM_RETURN_IF_ERROR(validate_stitching_backend_generation(config, expected_invalidation_id, updated));
    HM_RETURN_IF_ERROR(publish_game_config(game_dir, YAML::Dump(config) + "\n"));
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError("Unable to apply interactive rink leveling: " + std::string(exception.what()));
  }
  return updated;
}

std::string projection_crop_geometry(const std::string& pto, const StitchProjectionFraming& framing) {
  // PTO panorama dimensions and S bounds change when applying a crop/output
  // cap. Camera dimensions, lens parameters and poses remain in camera space.
  static const std::regex camera_geometry(R"(^(?:w|h|f|v|a|b|c|d|e|g|t|y|p|r|TrX|TrY|TrZ|Tpy|Tpp)[=+\-.0-9].*$)");
  static const std::regex panorama_geometry(R"(^(?:f|v|P)[=+\-.0-9].*$)");
  std::istringstream lines(pto);
  std::ostringstream result;
  result.imbue(std::locale::classic());
  result.precision(17);
  result << "crop-geometry-v1\n" << framing.auto_fov << ' ' << framing.horizontal_fov << ' ' << framing.auto_canvas;
  for (double angle : framing.rotation_degrees)
    result << ' ' << angle;
  result << '\n';
  size_t cameras = 0, panoramas = 0;
  for (std::string line; std::getline(lines, line);) {
    std::istringstream fields(line);
    std::string kind;
    fields >> kind;
    if (kind != "p" && kind != "i")
      continue;
    kind == "p" ? ++panoramas : ++cameras;
    result << kind;
    for (std::string field; fields >> field;) {
      if (kind == "p" && field.rfind("P\"", 0) == 0) {
        for (std::string rest; field.back() != '"' && fields >> rest;)
          field += " " + rest;
        result << ' ' << field;
      } else if (std::regex_match(field, kind == "p" ? panorama_geometry : camera_geometry)) {
        result << ' ' << field;
      }
    }
    result << '\n';
  }
  return cameras == 2 && panoramas == 1 ? result.str() : std::string();
}

bool projection_crop_reviewed(const YAML::Node& config, const std::string& geometry) {
  const auto ui = config["hstream_ui"];
  const auto review = ui && ui.IsMap() ? ui["projection_crop_geometry"] : YAML::Node();
  return !geometry.empty() && review && review.IsScalar() && review.as<std::string>() == geometry;
}

void write_projection_crop_review(YAML::Node& config, const std::string& geometry) {
  if (!geometry.empty())
    config["hstream_ui"]["projection_crop_geometry"] = geometry;
}

absl::StatusOr<StitchingBackendChoices> apply_stitching_crop_selection(
    const fs::path& game_dir,
    const std::string& expected_invalidation_id,
    const StitchingBackendChoices& expected_choices,
    const StitchProjectionFraming& framing,
    const std::string& geometry) {
  if (expected_choices.mapping_backend != "nona" || expected_invalidation_id.empty() || geometry.empty())
    return absl::InvalidArgumentError("Crop selection requires owned NONA calibration geometry");
  StitchingBackendChoices updated = expected_choices;
  updated.projection_framing.auto_crop = framing.auto_crop;
  updated.projection_framing.crop = framing.crop;
  HM_RETURN_IF_ERROR(validate_projection_view(updated.projection_framing));
  auto transaction = GameConfigTransactionLock::Acquire(game_dir);
  if (!transaction.ok())
    return transaction.status();
  try {
    YAML::Node config = YAML::LoadFile((game_dir / "config.yaml").string());
    HM_RETURN_IF_ERROR(validate_stitching_backend_generation(config, expected_invalidation_id, expected_choices));
    write_stitch_projection_framing(config, updated.projection_framing);
    write_projection_framing_node(
        config["hstream_ui"]["stitching_calibration"]["backend_generation"]["projection_framing"],
        updated.projection_framing);
    write_projection_crop_review(config, geometry);
    HM_RETURN_IF_ERROR(validate_stitching_backend_generation(config, expected_invalidation_id, updated));
    HM_RETURN_IF_ERROR(publish_game_config(game_dir, YAML::Dump(config) + "\n"));
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError("Unable to apply crop selection: " + std::string(exception.what()));
  }
  return updated;
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
