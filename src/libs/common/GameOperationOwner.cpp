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
#include "hstream/src/libs/common/ProcessGuard.h"
#include "yaml-cpp/yaml.h"

namespace hm::game_operation {
namespace {
constexpr size_t kMaximumOwnerBytes = 16384;
constexpr const char* kOwner = "game-operation.yaml";
std::mutex authorizations_mutex;
std::map<std::string, std::string> authorizations;
struct CommandProof {
  int descriptor{-1};
  std::filesystem::path path;
  std::string header;
  bool launched{false};
  bool exited{false};
  bool complete{false};
  ~CommandProof() {
    if (descriptor >= 0)
      ::close(descriptor);
  }
};
struct LocalAdmission {
  pid_t process_id{0};
  bool supervised{false};
  std::map<std::string, std::shared_ptr<CommandProof>> commands;
};
std::map<std::string, LocalAdmission> local_admissions;

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
    local_admissions.emplace(token, LocalAdmission{::getpid(), false, {}});
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
absl::StatusOr<LocalAdmission*> local_admission(const std::string& token) {
  const auto found = local_admissions.find(token);
  if (found == local_admissions.end() || found->second.process_id != ::getpid())
    return absl::FailedPreconditionError("No exact local game admission is registered");
  return &found->second;
}
absl::StatusOr<std::shared_ptr<CommandProof>> command_proof(const std::string& token, const std::string& id) {
  const auto admission = local_admission(token);
  if (!admission.ok())
    return admission.status();
  const auto found = (*admission)->commands.find(id);
  if (!(*admission)->supervised || found == (*admission)->commands.end())
    return absl::FailedPreconditionError("No exact guarded command is registered");
  return found->second;
}
absl::StatusOr<char> proof_state(CommandProof& proof) {
  if (proof.complete)
    return 'C';
  const auto state = process_guard::ReadReceipt(proof.descriptor, proof.path, proof.header);
  if (state.ok() && *state == 'C') {
    // The exact kernel completion proof is immutable once observed. Retire its
    // descriptor so large sequential experiment batches do not exhaust RLIMIT_NOFILE.
    proof.complete = true;
    ::close(proof.descriptor);
    proof.descriptor = -1;
    ::unlink(proof.path.c_str());
  }
  if (state.ok() && *state != 'C' && proof.exited)
    return absl::FailedPreconditionError("Supervisor exited without descendant cleanup proof; game needs recovery");
  return state;
}
} // namespace

absl::Status EnableIsolatedChildProof() {
  return process_guard::Enable();
}

absl::Status RetireIsolatedChildren(int64_t grace_ms, int64_t poll_ms) {
  return process_guard::Retire(grace_ms, poll_ms);
}

absl::Status EnableSupervisedChildProof(const std::string& token) {
  std::lock_guard<std::mutex> lock(authorizations_mutex);
  const auto admission = local_admission(token);
  if (!admission.ok())
    return admission.status();
  if (process_guard::Enabled())
    return absl::FailedPreconditionError("Cannot mix isolated and shared process scopes");
  (*admission)->supervised = true;
  return absl::OkStatus();
}

absl::StatusOr<SupervisedCommand> PrepareSupervisedCommand(
    const std::filesystem::path& game,
    const std::string& token,
    int64_t grace_ms,
    int64_t poll_ms,
    const std::vector<std::string>& command) {
  if (command.empty() || command.front().empty() || grace_ms <= 0 || poll_ms <= 0 || poll_ms > grace_ms ||
      grace_ms > 24 * 60 * 60 * 1000LL)
    return absl::InvalidArgumentError("Invalid guarded command or explicit cleanup timing");
  for (const auto& argument : command)
    if (argument.find('\0') != std::string::npos)
      return absl::InvalidArgumentError("Command contains NUL");
  std::lock_guard<std::mutex> lock(authorizations_mutex);
  const auto admission = local_admission(token);
  if (!admission.ok())
    return admission.status();
  const auto allowed = authorizations.find(key(game));
  if (!(*admission)->supervised || allowed == authorizations.end() || allowed->second != token)
    return absl::FailedPreconditionError("Guarded command does not belong to this game admission");
  const auto nonce = NewToken();
  if (!nonce.ok())
    return nonce.status();
  auto proof = std::make_shared<CommandProof>();
  proof->path = game_storage::StateDirectory(game) / (".game-operation-proof-" + *nonce);
  proof->header = process_guard::ReceiptHeader(token, *nonce);
  proof->descriptor = ::open(proof->path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (proof->descriptor < 0)
    return io("Cannot create guarded command permit");
  const std::string contents = proof->header + "0P";
  if (::write(proof->descriptor, contents.data(), contents.size()) != static_cast<ssize_t>(contents.size()) ||
      ::fsync(proof->descriptor) != 0) {
    const auto failed = io("Cannot persist guarded command permit");
    ::unlink(proof->path.c_str());
    return failed;
  }
  (*admission)->commands.emplace(*nonce, proof);
  SupervisedCommand result{
      *nonce,
      {"--receipt",
       proof->path.string(),
       "--token",
       token,
       "--nonce",
       *nonce,
       "--parent-pid",
       std::to_string(::getpid()),
       "--grace-ms",
       std::to_string(grace_ms),
       "--poll-ms",
       std::to_string(poll_ms),
       "--"}};
  result.arguments.insert(result.arguments.end(), command.begin(), command.end());
  return result;
}

absl::Status SupervisedCommandStarted(const std::string& token, const std::string& id) {
  std::lock_guard<std::mutex> lock(authorizations_mutex);
  const auto proof = command_proof(token, id);
  if (!proof.ok())
    return proof.status();
  (*proof)->launched = true;
  return absl::OkStatus();
}

absl::Status SupervisedCommandFinished(const std::string& token, const std::string& id) {
  std::lock_guard<std::mutex> lock(authorizations_mutex);
  const auto proof = command_proof(token, id);
  if (!proof.ok())
    return proof.status();
  (*proof)->exited = true;
  return absl::OkStatus();
}

absl::StatusOr<bool> SupervisedCommandStopped(const std::string& token, const std::string& id) {
  std::lock_guard<std::mutex> lock(authorizations_mutex);
  const auto proof = command_proof(token, id);
  if (!proof.ok())
    return proof.status();
  const auto state = proof_state(**proof);
  if (!state.ok())
    return state.status();
  if (*state == 'F')
    return absl::FailedPreconditionError("Process guard could not prove cleanup; game stays reserved");
  return *state == 'C';
}

absl::Status CancelUnlaunchedSupervisedCommand(const std::string& token, const std::string& id) {
  std::lock_guard<std::mutex> lock(authorizations_mutex);
  const auto proof = command_proof(token, id);
  if (!proof.ok())
    return proof.status();
  if ((*proof)->complete)
    return absl::FailedPreconditionError("Completed guard is not an unlaunched command");
  if (::flock((*proof)->descriptor, LOCK_EX | LOCK_NB) != 0)
    return absl::FailedPreconditionError("A supervisor has consumed this command permit");
  struct Unlock {
    int descriptor;
    ~Unlock() {
      ::flock(descriptor, LOCK_UN);
    }
  } unlock{(*proof)->descriptor};
  const auto state = process_guard::ReadReceipt((*proof)->descriptor, (*proof)->path, (*proof)->header);
  if (!state.ok())
    return state.status();
  if ((*proof)->launched || *state != 'P')
    return absl::FailedPreconditionError("Launched guard requires exact descendant completion proof");
  if (::unlink((*proof)->path.c_str()) != 0)
    return io("Cannot retire unlaunched guarded command");
  local_admissions.at(token).commands.erase(id);
  return absl::OkStatus();
}

absl::Status SignalSupervisedCommand(const std::string& token, const std::string& id, int signal) {
  std::lock_guard<std::mutex> lock(authorizations_mutex);
  const auto proof = command_proof(token, id);
  if (proof.ok() && (*proof)->complete)
    return absl::OkStatus();
  return proof.ok() ? process_guard::RequestSignal((*proof)->descriptor, (*proof)->path, (*proof)->header, signal)
                    : proof.status();
}

absl::StatusOr<bool> LocalChildrenStopped(const std::string& token) {
  std::lock_guard<std::mutex> lock(authorizations_mutex);
  const auto admission = local_admission(token);
  if (!admission.ok())
    return admission.status();
  if (process_guard::Enabled())
    return process_guard::Reap();
  if (!(*admission)->supervised)
    return absl::FailedPreconditionError("No kernel-backed local child scope is available; game stays reserved");
  bool complete = true;
  for (const auto& [id, proof] : (*admission)->commands) {
    const auto state = proof_state(*proof);
    if (!state.ok())
      return state.status();
    if (*state == 'F')
      return absl::FailedPreconditionError("Process guard could not prove cleanup; game stays reserved");
    complete &= *state == 'C';
  }
  return complete;
}

absl::Status SignalLocalChildren(const std::string& token, int signal) {
  std::lock_guard<std::mutex> lock(authorizations_mutex);
  const auto admission = local_admission(token);
  if (!admission.ok())
    return admission.status();
  if (process_guard::Enabled())
    return process_guard::Signal(signal);
  if (!(*admission)->supervised)
    return absl::FailedPreconditionError("No exact local child scope is available");
  for (const auto& [id, proof] : (*admission)->commands) {
    if (proof->complete)
      continue;
    const auto status = process_guard::RequestSignal(proof->descriptor, proof->path, proof->header, signal);
    if (!status.ok())
      return status;
  }
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
  if (result.ok()) {
    {
      std::lock_guard<std::mutex> guard(authorizations_mutex);
      const auto found = local_admissions.find(token);
      if (found != local_admissions.end())
        for (const auto& [id, proof] : found->second.commands)
          ::unlink(proof->path.c_str());
    }
    ForgetAuthorization(game, token);
  }
  return result;
}
} // namespace hm::game_operation
