#include "hstream/src/libs/common/GameOperationOwner.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#include "hstream/src/libs/common/GameStorage.h"
#include "yaml-cpp/yaml.h"

namespace hm::game_operation {
namespace {
constexpr size_t kMaximumOwnerBytes = 16384;
constexpr const char* kOwner = "game-operation.yaml";
std::mutex authorizations_mutex;
std::map<std::string, std::string> authorizations;
struct ProcessIdentity {
  pid_t pid;
  pid_t parent;
  uint64_t started;
  char state;
};
struct LocalAdmission {
  uint64_t started{0};
  std::optional<std::vector<ProcessIdentity>> ancestors;
};
std::optional<std::vector<ProcessIdentity>> capture_ancestors();
std::map<std::string, LocalAdmission> local_admissions;
uint64_t boot_ticks() {
  struct timespec now{};
  const long rate = ::sysconf(_SC_CLK_TCK);
  if (rate <= 0 || ::clock_gettime(CLOCK_BOOTTIME, &now) != 0)
    return 0;
  return uint64_t(now.tv_sec) * rate + uint64_t(now.tv_nsec) * rate / 1000000000ULL;
}

std::string key(const std::filesystem::path& game) {
  std::error_code error;
  auto resolved = std::filesystem::canonical(game, error);
  return error ? std::filesystem::absolute(game).lexically_normal().string() : resolved.string();
}

absl::Status io(const char* operation) {
  return absl::InternalError(std::string(operation) + ": " + std::strerror(errno));
}

bool identity(const std::string& value, size_t maximum = 160) {
  return !value.empty() && value.size() <= maximum &&
      value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:-") ==
      std::string::npos;
}

absl::Status validate(const Owner& owner) {
  if ((owner.kind != "workflow" && owner.kind != "pipeline" && owner.kind != "int8" && owner.kind != "mutation" &&
       owner.kind != "experiment") ||
      !identity(owner.id) || !identity(owner.token) || !identity(owner.host, 255) || !identity(owner.boot_id) ||
      owner.process_id < 0 || owner.revision == 0 || owner.workflow_directory.size() > 4096 ||
      owner.workflow_directory.find('\0') != std::string::npos ||
      (!owner.workflow_directory.empty() && !std::filesystem::path(owner.workflow_directory).is_absolute()))
    return absl::InvalidArgumentError("Invalid game operation owner");
  return absl::OkStatus();
}
} // namespace

Lock::~Lock() {
  if (descriptor_ >= 0) {
    ::flock(descriptor_, LOCK_UN);
    ::close(descriptor_);
  }
  if (directory_ >= 0)
    ::close(directory_);
}

absl::StatusOr<std::unique_ptr<Lock>> Lock::Acquire(const std::filesystem::path& game, bool wait) {
  auto status = game_storage::EnsureStateDirectory(game);
  if (!status.ok())
    return status;
  const int directory =
      ::open(game_storage::StateDirectory(game).c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (directory < 0)
    return io("Cannot open game admission directory");
  const int descriptor = ::openat(directory, ".game-operation.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    const auto error = io("Cannot open game admission lock");
    ::close(directory);
    return error;
  }
  auto lock = std::unique_ptr<Lock>(new Lock(directory, descriptor));
  struct stat metadata{};
  if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode))
    return absl::FailedPreconditionError("Game admission lock is not a regular file");
  while (::flock(descriptor, LOCK_EX | (wait ? 0 : LOCK_NB)) != 0) {
    if (errno == EINTR)
      continue;
    if (errno == EWOULDBLOCK || errno == EAGAIN)
      return absl::UnavailableError("Game admission is busy");
    return io("Cannot lock game admission");
  }
  return lock;
}

absl::StatusOr<std::optional<Owner>> Lock::Read() const {
  const int file = ::openat(directory_, kOwner, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0)
    return errno == ENOENT ? absl::StatusOr<std::optional<Owner>>(std::nullopt)
                           : absl::StatusOr<std::optional<Owner>>(io("Cannot read game admission"));
  struct Close {
    int fd;
    ~Close() {
      ::close(fd);
    }
  } close{file};
  struct stat before{}, after{}, final{};
  if (::fstat(file, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size <= 0 ||
      before.st_size > static_cast<off_t>(kMaximumOwnerBytes))
    return absl::FailedPreconditionError("Game operation owner is not a bounded regular file");
  std::string contents(static_cast<size_t>(before.st_size), '\0');
  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t count = ::read(file, contents.data() + offset, contents.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return absl::AbortedError("Game operation owner changed while reading");
    offset += static_cast<size_t>(count);
  }
  if (::fstat(file, &after) != 0 || ::fstatat(directory_, kOwner, &final, AT_SYMLINK_NOFOLLOW) != 0 ||
      before.st_dev != after.st_dev || before.st_ino != after.st_ino || before.st_size != after.st_size ||
      before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
      before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
      after.st_dev != final.st_dev || after.st_ino != final.st_ino)
    return absl::AbortedError("Game operation owner changed while reading");
  try {
    const auto node = YAML::Load(contents);
    if (!node.IsMap() || node.size() != 11 || node["schema"].as<int>() != 1)
      return absl::FailedPreconditionError("Unsupported game operation owner; recovery is required");
    Owner result{
        node["kind"].as<std::string>(),
        node["id"].as<std::string>(),
        node["token"].as<std::string>(),
        node["host"].as<std::string>(),
        node["boot_id"].as<std::string>(),
        node["workflow_directory"].as<std::string>(),
        node["process_id"].as<int64_t>(),
        node["revision"].as<uint64_t>(),
        node["cancel_requested"].as<bool>()};
    if (node["game_admission"].as<std::string>() != "exclusive")
      return absl::FailedPreconditionError("Invalid game admission mode");
    const auto valid = validate(result);
    if (!valid.ok())
      return valid;
    return std::optional<Owner>(std::move(result));
  } catch (const YAML::Exception& error) {
    return absl::FailedPreconditionError(
        std::string("Unreadable game operation owner; recovery is required: ") + error.what());
  }
}

absl::Status Lock::Write(const Owner& owner) const {
  const auto valid = validate(owner);
  if (!valid.ok())
    return valid;
  YAML::Node node;
  node["schema"] = 1;
  node["game_admission"] = "exclusive";
  node["kind"] = owner.kind;
  node["id"] = owner.id;
  node["token"] = owner.token;
  node["host"] = owner.host;
  node["boot_id"] = owner.boot_id;
  node["workflow_directory"] = owner.workflow_directory;
  node["process_id"] = owner.process_id;
  node["revision"] = owner.revision;
  node["cancel_requested"] = owner.cancel_requested;
  const std::string contents = YAML::Dump(node) + '\n';
  auto nonce = NewToken();
  if (!nonce.ok())
    return nonce.status();
  const std::string pending = ".game-operation-" + *nonce;
  const int fd = ::openat(directory_, pending.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0)
    return io("Cannot stage game admission");
  struct Cleanup {
    int fd, directory;
    std::string name;
    ~Cleanup() {
      ::close(fd);
      ::unlinkat(directory, name.c_str(), 0);
    }
  } cleanup{fd, directory_, pending};
  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t count = ::write(fd, contents.data() + offset, contents.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return io("Cannot write game admission");
    offset += static_cast<size_t>(count);
  }
  if (::fsync(fd) != 0 || ::renameat(directory_, pending.c_str(), directory_, kOwner) != 0 || ::fsync(directory_) != 0)
    return io("Cannot publish game admission");
  return absl::OkStatus();
}

absl::Status Lock::Authorize(const std::string& token) const {
  auto owner = Read();
  if (!owner.ok())
    return owner.status();
  if (*owner && (token.empty() || (**owner).token != token))
    return absl::FailedPreconditionError(
        "Game is owned by " + (**owner).kind + " " + (**owner).id +
        "; wait for confirmed completion or cancellation cleanup");
  return absl::OkStatus();
}

absl::Status Lock::Claim(Owner& owner) {
  auto current = Read();
  if (!current.ok())
    return current.status();
  if (*current)
    return absl::AlreadyExistsError("Game already has an active " + (**current).kind + " owner");
  owner.revision = 1;
  return Write(owner);
}

absl::Status Lock::Handoff(Owner& successor, const std::string& token, uint64_t revision) {
  auto current = Read();
  if (!current.ok())
    return current.status();
  if (!*current || (**current).token != token || (**current).revision != revision || (**current).cancel_requested)
    return absl::AbortedError("Game admission changed before handoff");
  if (successor.id != (**current).id || successor.kind != (**current).kind || successor.token == token)
    return absl::InvalidArgumentError("Game handoff requires the same operation and a new exact token");
  if (revision == std::numeric_limits<uint64_t>::max())
    return absl::ResourceExhaustedError("Game admission revision exhausted");
  successor.revision = revision + 1;
  return Write(successor);
}

absl::Status Lock::RequestCancel(const std::string& token, uint64_t revision) {
  auto current = Read();
  if (!current.ok())
    return current.status();
  if (!*current || (**current).token != token || (**current).revision != revision)
    return absl::AbortedError("Game admission changed before cancellation");
  Owner changed = **current;
  if (changed.revision == std::numeric_limits<uint64_t>::max())
    return absl::ResourceExhaustedError("Game admission revision exhausted");
  changed.cancel_requested = true;
  ++changed.revision;
  return Write(changed);
}

absl::Status Lock::Release(const std::string& token, uint64_t revision, bool stopped_and_clean) {
  if (!stopped_and_clean)
    return absl::FailedPreconditionError("Game owner remains in recovery until shutdown and cleanup are verified");
  auto current = Read();
  if (!current.ok())
    return current.status();
  if (!*current || (**current).token != token || (**current).revision != revision)
    return absl::AbortedError("Game admission changed before release");
  if (::unlinkat(directory_, kOwner, 0) != 0 || ::fsync(directory_) != 0)
    return io("Cannot release game admission");
  return absl::OkStatus();
}

absl::StatusOr<std::optional<Owner>> Inspect(const std::filesystem::path& game) {
  std::error_code error;
  if (!std::filesystem::exists(game_storage::StateDirectory(game), error) && !error)
    return std::optional<Owner>{};
  auto lock = Lock::Acquire(game, false);
  return lock.ok() ? (*lock)->Read() : absl::StatusOr<std::optional<Owner>>(lock.status());
}

absl::StatusOr<std::unique_ptr<Lock>> Mutation(const std::filesystem::path& game, bool wait) {
  auto lock = Lock::Acquire(game, wait);
  if (!lock.ok())
    return lock.status();
  const auto allowed = (*lock)->Authorize(Authorization(game));
  if (!allowed.ok())
    return allowed;
  return std::move(*lock);
}

absl::StatusOr<std::string> NewToken() {
  unsigned char bytes[24];
  size_t offset = 0;
  while (offset < sizeof(bytes)) {
    const ssize_t count = ::getrandom(bytes + offset, sizeof(bytes) - offset, 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return io("Cannot create game admission identity");
    offset += static_cast<size_t>(count);
  }
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (unsigned char value : bytes) {
    result.push_back(digits[value >> 4]);
    result.push_back(digits[value & 15]);
  }
  return result;
}

Owner LocalOwner(const std::string& kind, const std::string& id, const std::string& token) {
  Owner result;
  result.kind = kind;
  result.id = id;
  result.token = token;
  char host[256]{};
  if (::gethostname(host, sizeof(host) - 1) == 0)
    result.host = host;
  std::ifstream boot("/proc/sys/kernel/random/boot_id");
  std::getline(boot, result.boot_id);
  result.process_id = ::getpid();
  return result;
}

void RegisterAuthorization(const std::filesystem::path& game, const std::string& token) {
  std::lock_guard<std::mutex> lock(authorizations_mutex);
  authorizations[key(game)] = token;
  if (!local_admissions.count(token))
    local_admissions.emplace(token, LocalAdmission{boot_ticks(), capture_ancestors()});
}

void ForgetAuthorization(const std::filesystem::path& game, const std::string& token) {
  std::lock_guard<std::mutex> lock(authorizations_mutex);
  const auto found = authorizations.find(key(game));
  if (found != authorizations.end() && found->second == token)
    authorizations.erase(found);
  local_admissions.erase(token);
}

std::string Authorization(const std::filesystem::path& game) {
  const auto path = key(game);
  {
    std::lock_guard<std::mutex> lock(authorizations_mutex);
    const auto found = authorizations.find(path);
    if (found != authorizations.end())
      return found->second;
  }
  const char* inherited_game = std::getenv("HSTREAM_GAME_OPERATION_GAME");
  const char* inherited_token = std::getenv("HSTREAM_GAME_OPERATION_TOKEN");
  return inherited_game && inherited_token && key(inherited_game) == path ? inherited_token : "";
}
namespace {
struct ProcessPins {
  std::vector<int> descriptors;
  ~ProcessPins() {
    for (int fd : descriptors)
      ::close(fd);
  }
};

absl::StatusOr<std::string> proc_file(pid_t pid, const char* name) {
  const auto path = "/proc/" + std::to_string(pid) + "/" + name;
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return errno == ENOENT || errno == ESRCH ? absl::NotFoundError("Process exited") : io("Cannot inspect process");
  struct Close {
    int fd;
    ~Close() {
      ::close(fd);
    }
  } close{fd};
  std::string result;
  char buffer[4096];
  for (;;) {
    const auto count = ::read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0)
      return io("Cannot read process identity");
    if (!count)
      return result;
    result.append(buffer, count);
    if (result.size() > 65536)
      return absl::ResourceExhaustedError("Process metadata exceeds ownership bound");
  }
}

absl::StatusOr<ProcessIdentity> process_identity(pid_t pid) {
  const auto value = proc_file(pid, "stat");
  if (!value.ok())
    return value.status();
  const auto end_name = value->rfind(')');
  if (end_name == std::string::npos)
    return absl::UnavailableError("Invalid process identity");
  std::istringstream fields(value->substr(end_name + 2));
  ProcessIdentity result{pid, 0, 0, 0};
  std::string field;
  for (int index = 3; index <= 22; ++index) {
    if (!(fields >> field))
      return absl::UnavailableError("Incomplete process identity");
    if (index == 3)
      result.state = field.front();
    if (index == 4)
      result.parent = static_cast<pid_t>(std::stol(field));
    if (index == 22)
      result.started = std::stoull(field);
  }
  return result;
}

absl::StatusOr<bool> same_user(pid_t pid) {
  const auto value = proc_file(pid, "status");
  if (!value.ok())
    return value.status();
  std::istringstream lines(*value);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("Uid:", 0) != 0)
      continue;
    std::istringstream fields(line.substr(4));
    bool matches = false;
    for (int index = 0; index < 4; ++index) {
      uint64_t uid;
      if (!(fields >> uid))
        return absl::UnavailableError("Invalid process credentials");
      matches |= uid == ::geteuid();
    }
    return matches;
  }
  return absl::UnavailableError("Process credentials are unavailable");
}

bool same_identity(const ProcessIdentity& left, const ProcessIdentity& right) {
  return left.pid == right.pid && left.started == right.started;
}

#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
absl::StatusOr<bool> process_exited(int descriptor) {
  struct pollfd watched{descriptor, POLLIN, 0};
  int result;
  do {
    result = ::poll(&watched, 1, 0);
  } while (result < 0 && errno == EINTR);
  if (result < 0)
    return io("Cannot inspect pinned process liveness");
  if (watched.revents & (POLLERR | POLLNVAL))
    return absl::UnavailableError("Pinned process liveness is unavailable");
  return (watched.revents & (POLLIN | POLLHUP)) != 0;
}

absl::StatusOr<ProcessIdentity> pin_process(pid_t pid, ProcessPins& pins) {
  const int fd = ::syscall(SYS_pidfd_open, pid, 0);
  if (fd < 0)
    return io("Cannot pin process ancestry");
  pins.descriptors.push_back(fd);
  const auto identity = process_identity(pid);
  if (!identity.ok())
    return identity.status();
  const auto exited = process_exited(fd);
  if (!exited.ok())
    return exited.status();
  if (*exited)
    return absl::UnavailableError("Process ancestry changed while pinning");
  return identity;
}

bool stable_chain(const std::vector<ProcessIdentity>& chain, const ProcessPins& pins) {
  if (chain.size() != pins.descriptors.size())
    return false;
  for (size_t index = 0; index < chain.size(); ++index) {
    const auto current = process_identity(chain[index].pid);
    const auto exited = process_exited(pins.descriptors[index]);
    if (!current.ok() || !same_identity(*current, chain[index]) || current->parent != chain[index].parent ||
        !exited.ok() || *exited)
      return false;
  }
  return true;
}
#endif

std::optional<std::vector<ProcessIdentity>> capture_ancestors() {
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
  try {
    ProcessPins pins;
    std::vector<ProcessIdentity> chain;
    pid_t pid = ::getpid();
    for (size_t depth = 0; depth < 128 && pid > 0; ++depth) {
      const auto current = pin_process(pid, pins);
      if (!current.ok() || (!chain.empty() && current->started > chain.back().started))
        return std::nullopt;
      chain.push_back(*current);
      pid = current->parent;
    }
    if (pid != 0 || !stable_chain(chain, pins))
      return std::nullopt;
    return chain;
  } catch (const std::exception&) {
    return std::nullopt;
  }
#else
  return std::nullopt;
#endif
}

bool proven_unrelated(const ProcessIdentity& candidate, const LocalAdmission& admission) {
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
  if (!admission.ancestors || admission.ancestors->empty())
    return false;
  try {
    ProcessPins pins;
    std::vector<ProcessIdentity> chain;
    pid_t pid = candidate.pid;
    for (size_t depth = 0; depth < 128 && pid > 1; ++depth) {
      const auto current = pin_process(pid, pins);
      if (!current.ok() || (chain.empty() && !same_identity(*current, candidate)) ||
          (!chain.empty() && current->started > chain.back().started))
        return false;
      chain.push_back(*current);
      if (std::any_of(admission.ancestors->begin(), admission.ancestors->end(), [&](const auto& ancestor) {
            return same_identity(ancestor, *current);
          }))
        return false;
      if (current->started < admission.started)
        return stable_chain(chain, pins);
      pid = current->parent;
    }
  } catch (const std::exception&) {
    return false;
  }
#endif
  // Orphans, original ancestors/subreapers and uncertain traversals may still
  // contain an owned detached helper. They cannot provide exclusion evidence.
  return false;
}

absl::Status inspect_children(const std::string& token, ProcessPins& pins) {
  if (!identity(token))
    return absl::InvalidArgumentError("Missing process capability");
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
  try {
    LocalAdmission admission;
    {
      std::lock_guard<std::mutex> lock(authorizations_mutex);
      const auto found = local_admissions.find(token);
      if (found != local_admissions.end())
        admission = found->second;
    }
    if (!admission.started)
      return absl::FailedPreconditionError("No local process admission epoch is available");
    const std::string expected = "HSTREAM_GAME_OPERATION_TOKEN=" + token;
    bool saw_self = false;
    for (const auto& entry : std::filesystem::directory_iterator("/proc")) {
      const std::string name = entry.path().filename().string();
      if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
        continue;
      const auto pid = static_cast<pid_t>(std::stol(name));
      if (pid == ::getpid()) {
        saw_self = true;
        continue;
      }
      // Unrelated preexisting processes cannot have inherited this newly
      // created capability. Their /proc environments may legitimately be
      // unreadable (for example setuid desktop services).
      const auto identity = process_identity(pid);
      if (!identity.ok()) {
        if (absl::IsNotFound(identity.status()))
          continue;
        return identity.status();
      }
      if (identity->started < admission.started || identity->state == 'Z' || identity->state == 'X')
        continue;
      const auto eligible = same_user(pid);
      if (!eligible.ok()) {
        if (absl::IsNotFound(eligible.status()))
          continue;
        return eligible.status();
      }
      if (!*eligible)
        continue;
      const int pin = ::syscall(SYS_pidfd_open, pid, 0);
      if (pin < 0) {
        if (errno == ESRCH)
          continue;
        return io("Cannot pin process identity");
      }
      struct Close {
        int fd;
        ~Close() {
          if (fd >= 0)
            ::close(fd);
        }
      } pinned{pin};
      const auto current = process_identity(pid);
      if (!current.ok() || !same_identity(*current, *identity)) {
        const auto exited = process_exited(pin);
        if (exited.ok() && *exited)
          continue;
        return absl::UnavailableError("Process identity changed before ownership inspection");
      }
      const int env = ::open((entry.path() / "environ").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
      if (env < 0) {
        const int access_error = errno;
        const auto exited = process_exited(pin);
        if (exited.ok() && *exited)
          continue;
        // Exit can race the first stat read. Zombies have released their files
        // and GPU resources, but their environ is unreadable until reaped.
        const auto final = process_identity(pid);
        if (final.ok() && same_identity(*final, *identity) && (final->state == 'Z' || final->state == 'X'))
          continue;
        if ((access_error == EACCES || access_error == EPERM) && proven_unrelated(*identity, admission))
          continue;
        errno = access_error;
        return io(("Cannot prove local helper ownership for process " + std::to_string(pid)).c_str());
      }
      Close environment{env};
      std::string contents;
      char buffer[4096];
      while (true) {
        const ssize_t count = ::read(env, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
          continue;
        if (count < 0)
          return io("Cannot read local helper ownership");
        if (!count)
          break;
        contents.append(buffer, count);
        if (contents.size() > 1024 * 1024)
          return absl::ResourceExhaustedError("Process environment exceeds ownership bound");
      }
      const auto exited = process_exited(pin);
      if (!exited.ok())
        return exited.status();
      if (*exited)
        continue;
      bool owned = false;
      for (size_t offset = 0; offset < contents.size();) {
        const size_t end = contents.find('\0', offset);
        if (contents.substr(offset, end == std::string::npos ? end : end - offset) == expected)
          owned = true;
        if (end == std::string::npos)
          break;
        offset = end + 1;
      }
      if (owned) {
        pins.descriptors.push_back(pin);
        pinned.fd = -1;
      }
    }
    if (!saw_self)
      return absl::UnavailableError("Local process inventory is incomplete");
    return absl::OkStatus();
  } catch (const std::exception& error) {
    return absl::UnavailableError("Cannot enumerate local helpers: " + std::string(error.what()));
  }
#else
  return absl::UnimplementedError("Local game ownership requires Linux pidfd support");
#endif
}
} // namespace

absl::StatusOr<bool> LocalChildrenStopped(const std::string& token) {
  ProcessPins pins;
  auto status = inspect_children(token, pins);
  if (!status.ok())
    return status;
  return pins.descriptors.empty();
}
absl::Status SignalLocalChildren(const std::string& token, int signal) {
  ProcessPins pins;
  auto status = inspect_children(token, pins);
  if (!status.ok())
    return status;
#if defined(SYS_pidfd_send_signal)
  for (int fd : pins.descriptors)
    if (::syscall(SYS_pidfd_send_signal, fd, signal, nullptr, 0) != 0 && errno != ESRCH)
      return io("Cannot signal owned helper");
#endif
  return absl::OkStatus();
}
absl::Status ReleaseLocal(const std::filesystem::path& game, const std::string& token, bool cleanup_complete) {
  if (!cleanup_complete)
    return absl::FailedPreconditionError("Local cleanup is incomplete");
  auto stopped = LocalChildrenStopped(token);
  if (!stopped.ok())
    return stopped.status();
  if (!*stopped)
    return absl::FailedPreconditionError("Local helpers remain alive; game stays reserved");
  auto lock = Lock::Acquire(game);
  if (!lock.ok())
    return lock.status();
  auto owner = (*lock)->Read();
  if (!owner.ok())
    return owner.status();
  if (!*owner || (**owner).token != token)
    return absl::AbortedError("Local game owner changed");
  auto result = (*lock)->Release(token, (**owner).revision, true);
  if (result.ok())
    ForgetAuthorization(game, token);
  return result;
}
} // namespace hm::game_operation
