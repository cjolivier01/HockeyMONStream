#include "hstream/src/libs/common/GameStorage.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "yaml-cpp/yaml.h"

namespace hm::game_storage {
namespace {
namespace fs = std::filesystem;
constexpr char kDirectory[] = "_hockeymon";
constexpr char kManifest[] = "manifest.json";
constexpr char kLock[] = ".manifest.lock";
constexpr char kTemporary[] = ".manifest.tmp-";
constexpr size_t kMaxManifestBytes = 32 * 1024 * 1024;
constexpr size_t kMaxEntries = 100000;
constexpr unsigned kMaxDepth = 64;

struct Descriptor {
  int fd{-1};
  explicit Descriptor(int value) : fd(value) {}
  ~Descriptor() {
    if (fd >= 0)
      ::close(fd);
  }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
};

absl::Status Failure(const std::string& operation) {
  return absl::InternalError(operation + ": " + std::strerror(errno));
}

// True when path's last component is the private directory itself. A trailing
// separator leaves filename() empty, so fall back to the parent's name.
// Only the last component: a stitching experiment's candidate game really does
// live at <game>/_hockeymon/stitching-experiments/sessions/<session>/<name>,
// and rejecting any _hockeymon component would refuse its own state directory.
bool NamesStateDirectory(const fs::path& path) {
  const fs::path normalized = path.lexically_normal();
  const fs::path name = normalized.filename();
  return (name.empty() ? normalized.parent_path().filename() : name) == kDirectory;
}

bool Relative(const fs::path& path) {
  if (path.empty() || path.is_absolute())
    return false;
  for (const auto& part : path)
    if (part == ".." || part == "." || part.empty())
      return false;
  return true;
}

absl::Status CheckDirectory(int fd) {
  struct stat st{};
  if (fd < 0 || ::fstat(fd, &st) != 0)
    return Failure("Open game state directory");
  if (!S_ISDIR(st.st_mode))
    return absl::FailedPreconditionError("Game state is not a directory");
  return absl::OkStatus();
}

// Open each owned directory without following links. The game root may be on
// mounted storage, but _hockeymon must belong to that root, not another game.
int OpenState(int game_fd) {
  return ::openat(game_fd, kDirectory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

struct Entry {
  std::string path, kind, role, producer, run_id;
  uint64_t size_bytes{};
};
using Entries = std::map<std::string, Entry>;

std::string Role(const std::string& path) {
  for (const auto& component : fs::path(path)) {
    const std::string name = component.string();
    if (name.find("journal") != std::string::npos || name.find("recovery") != std::string::npos ||
        name.find(".lock") != std::string::npos || name.find("hstream-job-") == 0 ||
        name.find("hstream-finalize-") != std::string::npos)
      return "recovery";
    if (name == "hstream-runtime" || name == "calibration-frame-inspection" || name == "player-frame-inspection" ||
        name.find("play_tracker_config") == 0)
      return "cache";
    if (name.find(".manifest.tmp-") == 0 || name.find("hstream-stitch-") == 0 || name.find("hstream-rink-") == 0 ||
        name.find("hstream-calibration-input-") == 0 || name.find("hstream-field-mask-input-") == 0 || name == "work" ||
        name.find(".highlights-export-") == 0 || name.find("hstream-control-mask-snapshot-") == 0)
      return "temporary";
  }
  // Conservative: unrecognized internal state is retained, not declared cache.
  return "saved-state";
}

absl::Status Read(int state_fd, Entries* entries) {
  Descriptor input(::openat(state_fd, kManifest, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  if (input.fd < 0)
    return errno == ENOENT ? absl::OkStatus() : Failure("Read game manifest");
  struct stat st{};
  if (::fstat(input.fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
      static_cast<uint64_t>(st.st_size) > kMaxManifestBytes)
    return absl::FailedPreconditionError("Game manifest must be a bounded regular file");
  std::string bytes;
  char buffer[8192];
  for (;;) {
    const ssize_t count = ::read(input.fd, buffer, sizeof(buffer));
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return Failure("Read game manifest");
    }
    if (count == 0)
      break;
    bytes.append(buffer, count);
    if (bytes.size() > kMaxManifestBytes)
      return absl::ResourceExhaustedError("Game manifest is too large");
  }
  try {
    const YAML::Node document = YAML::Load(bytes);
    if (!document.IsMap() || document["schema_version"].as<int>() != 1 ||
        document["layout"].as<std::string>() != "hockeymon-game-state-v1" || !document["files"].IsSequence() ||
        document["files"].size() > kMaxEntries)
      return absl::FailedPreconditionError("Unsupported game manifest");
    for (const auto& node : document["files"]) {
      Entry entry;
      entry.path = node["path"].as<std::string>();
      entry.kind = node["kind"].as<std::string>();
      entry.role = node["role"].as<std::string>();
      entry.producer = node["producer"].as<std::string>();
      if (node["run_id"])
        entry.run_id = node["run_id"].as<std::string>();
      if (!Relative(entry.path) || (entry.kind != "file" && entry.kind != "directory" && entry.kind != "symlink"))
        return absl::FailedPreconditionError("Invalid game manifest entry");
      if (!entries->emplace(entry.path, std::move(entry)).second)
        return absl::FailedPreconditionError("Duplicate game manifest path");
    }
  } catch (const std::exception& error) {
    return absl::FailedPreconditionError(std::string("Invalid game manifest: ") + error.what());
  }
  return absl::OkStatus();
}

absl::Status Scan(
    int directory_fd,
    const fs::path& relative,
    const Entries& previous,
    Entries* entries,
    const std::string& producer,
    unsigned depth) {
  if (depth > kMaxDepth)
    return absl::ResourceExhaustedError("Game inventory exceeds directory depth limit");
  DIR* directory = ::fdopendir(::dup(directory_fd));
  if (!directory)
    return Failure("Read game state directory");
  struct Close {
    DIR* directory;
    ~Close() {
      ::closedir(directory);
    }
  } close{directory};
  for (;;) {
    errno = 0;
    const auto* child = ::readdir(directory);
    if (!child)
      return errno ? Failure("Read game state directory") : absl::OkStatus();
    const std::string name(child->d_name);
    if (name == "." || name == ".." || (depth == 0 && name == kManifest))
      continue;
    struct stat st{};
    if (::fstatat(directory_fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW)) {
      if (errno == ENOENT)
        continue; // Another operation finished cleanup.
      return Failure("Inspect game artifact");
    }
    Entry entry;
    entry.path = (relative / name).generic_string();
    entry.kind = S_ISREG(st.st_mode) ? "file"
        : S_ISDIR(st.st_mode)        ? "directory"
        : S_ISLNK(st.st_mode)        ? "symlink"
                                     : "";
    if (entry.kind.empty())
      return absl::FailedPreconditionError("Unsupported special file in game state: " + entry.path);
    entry.role = Role(entry.path);
    entry.producer = producer;
    const auto old = previous.find(entry.path);
    if (old != previous.end()) {
      entry.producer = old->second.producer;
      entry.run_id = old->second.run_id;
      if (old->second.role == "input-reference")
        entry.role = "input-reference";
    }
    if (S_ISREG(st.st_mode))
      entry.size_bytes = st.st_size;
    entries->emplace(entry.path, entry);
    if (entries->size() > kMaxEntries)
      return absl::ResourceExhaustedError("Too many game artifacts");
    if (S_ISDIR(st.st_mode)) {
      Descriptor nested(::openat(directory_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (nested.fd < 0) {
        if (errno == ENOENT)
          continue;
        return Failure("Open game artifact directory");
      }
      const auto status = Scan(nested.fd, entry.path, previous, entries, producer, depth + 1);
      if (!status.ok())
        return status;
    }
  }
}

std::string JsonString(const std::string& text) {
  static const char hex[] = "0123456789abcdef";
  std::string result = "\"";
  for (const unsigned char value : text) {
    if (value == '"' || value == '\\') {
      result += '\\';
      result += static_cast<char>(value);
    } else if (value < 0x20) {
      result += "\\u00";
      result += hex[value >> 4];
      result += hex[value & 15];
    } else {
      result += static_cast<char>(value);
    }
  }
  return result + "\"";
}

absl::Status Write(int state_fd, const Entries& entries, const std::string& producer) {
  if (entries.size() > kMaxEntries)
    return absl::ResourceExhaustedError("Too many game artifacts");
  std::ostringstream out;
  out << "{\"schema_version\":1,\"layout\":\"hockeymon-game-state-v1\",\"updated_by\":" << JsonString(producer)
      << ",\"files\":[";
  bool first = true;
  for (const auto& [path, entry] : entries) {
    if (!first)
      out << ',';
    first = false;
    out << "\n{\"path\":" << JsonString(path) << ",\"kind\":" << JsonString(entry.kind)
        << ",\"role\":" << JsonString(entry.role) << ",\"producer\":" << JsonString(entry.producer)
        << ",\"state\":\"present\"";
    if (entry.kind == "file")
      out << ",\"size_bytes\":" << entry.size_bytes;
    if (!entry.run_id.empty())
      out << ",\"run_id\":" << JsonString(entry.run_id);
    out << '}';
  }
  out << "\n]}\n";
  const std::string bytes = out.str();
  if (bytes.size() > kMaxManifestBytes)
    return absl::ResourceExhaustedError("Game manifest exceeds document limit");
  std::string name;
  Descriptor output(-1);
  for (unsigned attempt = 0; attempt < 128; ++attempt) {
    name = std::string(kTemporary) + std::to_string(::getpid()) + "-" + std::to_string(attempt);
    output.fd = ::openat(state_fd, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (output.fd >= 0)
      break;
    if (errno != EEXIST)
      return Failure("Create game manifest update");
  }
  if (output.fd < 0)
    return Failure("Create game manifest update");
  struct Remove {
    int fd;
    const std::string& name;
    ~Remove() {
      ::unlinkat(fd, name.c_str(), 0);
    }
  } remove{state_fd, name};
  size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count = ::write(output.fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return Failure("Write game manifest");
    offset += count;
  }
  if (::fsync(output.fd))
    return Failure("Sync game manifest");
  if (::renameat(state_fd, name.c_str(), state_fd, kManifest))
    return Failure("Publish game manifest");
  if (::fsync(state_fd))
    return Failure("Sync game state directory");
  return absl::OkStatus();
}

absl::Status InspectRegistered(int game_fd, Entry* entry, bool published) {
  const fs::path path(entry->path);
  if (!Relative(path))
    return absl::InvalidArgumentError("Registered game artifact must be relative and confined");
  if (published && *path.begin() == kDirectory)
    return absl::InvalidArgumentError("Published game artifact must be outside private state");
  Descriptor root(::dup(game_fd));
  int current = root.fd;
  std::vector<int> directories;
  struct Close {
    std::vector<int>& fds;
    ~Close() {
      for (int fd : fds)
        ::close(fd);
    }
  } close{directories};
  for (const auto& part : path.parent_path()) {
    const int next = ::openat(current, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (next < 0)
      return errno == ENOENT ? absl::NotFoundError("Registered game artifact is absent")
                             : Failure("Open published artifact parent");
    directories.push_back(next);
    current = next;
  }
  struct stat st{};
  if (::fstatat(current, path.filename().c_str(), &st, AT_SYMLINK_NOFOLLOW))
    return errno == ENOENT ? absl::NotFoundError("Registered game artifact is absent")
                           : Failure("Inspect published artifact");
  if (published && !S_ISREG(st.st_mode))
    return absl::FailedPreconditionError("Published artifact is not a regular file");
  entry->kind = S_ISREG(st.st_mode) ? "file" : S_ISDIR(st.st_mode) ? "directory" : S_ISLNK(st.st_mode) ? "symlink" : "";
  if (entry->kind.empty())
    return absl::FailedPreconditionError("Unsupported registered game artifact");
  entry->role = published ? "published" : "input-reference";
  if (S_ISREG(st.st_mode))
    entry->size_bytes = st.st_size;
  return absl::OkStatus();
}

absl::Status Update(const fs::path& game, const std::string& producer, Entry* published) {
  const auto ensured = EnsureStateDirectory(game);
  if (!ensured.ok())
    return ensured;
  Descriptor game_fd(::open(game.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (game_fd.fd < 0)
    return Failure("Open game directory");
  Descriptor state_fd(OpenState(game_fd.fd));
  auto checked = CheckDirectory(state_fd.fd);
  if (!checked.ok())
    return checked;
  Descriptor lock(::openat(state_fd.fd, kLock, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600));
  struct stat st{};
  if (lock.fd < 0 || ::fstat(lock.fd, &st) || !S_ISREG(st.st_mode))
    return absl::FailedPreconditionError("Cannot open game inventory lock");
  while (::flock(lock.fd, LOCK_EX)) {
    if (errno != EINTR)
      return Failure("Lock game inventory");
  }
  Entries previous, entries;
  auto status = Read(state_fd.fd, &previous);
  if (!status.ok())
    return status;
  status = Scan(state_fd.fd, kDirectory, previous, &entries, producer, 0);
  if (!status.ok())
    return status;
  for (auto& [path, entry] : previous) {
    if (entry.role != "published" && entry.role != "input-reference")
      continue;
    if (*fs::path(path).begin() == kDirectory)
      continue; // Already inventoried without following links.
    status = InspectRegistered(game_fd.fd, &entry, entry.role == "published");
    if (!status.ok()) {
      if (absl::IsNotFound(status))
        continue; // A removed path no longer belongs in the current inventory.
      return status;
    }
    entries.emplace(path, entry);
  }
  if (published) {
    status = InspectRegistered(game_fd.fd, published, published->role == "published");
    if (!status.ok())
      return status;
    entries[published->path] = *published;
  }
  return Write(state_fd.fd, entries, producer);
}
} // namespace

std::filesystem::path StateDirectory(const std::filesystem::path& game) {
  return game / kDirectory;
}

std::filesystem::path StatePath(const std::filesystem::path& game, const std::filesystem::path& relative) {
  if (!Relative(relative))
    throw std::invalid_argument("Game state paths must be relative and confined");
  return StateDirectory(game) / relative;
}

absl::Status EnsureStateDirectory(const std::filesystem::path& game) {
  // A caller that already resolved the private directory and then passed it on
  // as the game would otherwise mkdir a second one inside it. Nothing reads the
  // nested copy, so the locks and journals written there are silently private
  // to that one call; refuse instead of leaving a stray directory behind.
  if (NamesStateDirectory(game))
    return absl::InvalidArgumentError("Expected a game root, not its own state directory: " + game.string());
  Descriptor root(::open(game.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (root.fd < 0)
    return Failure("Open game directory");
  if (::mkdirat(root.fd, kDirectory, 0700) != 0 && errno != EEXIST)
    return Failure("Create game state directory");
  Descriptor state(OpenState(root.fd));
  return CheckDirectory(state.fd);
}

absl::Status RefreshManifest(const std::filesystem::path& game, const std::string& producer) {
  return Update(game, producer, nullptr);
}

absl::StatusOr<fs::path> ConfineWorkDirectory(const fs::path& game, const fs::path& requested) {
  // Checked up front rather than only through EnsureStateDirectory below: a
  // request that resolves outside the game returns before reaching it, so a
  // caller with the wrong game would otherwise be told it is fine.
  if (NamesStateDirectory(game))
    return absl::InvalidArgumentError("Expected a game root, not its own state directory: " + game.string());
  if (game.empty())
    return requested;
  const auto requested_relative =
      fs::absolute(requested).lexically_normal().lexically_relative(fs::absolute(game).lexically_normal());
  if (!requested_relative.empty() && *requested_relative.begin() == kDirectory) {
    const auto ensured = EnsureStateDirectory(game);
    if (!ensured.ok())
      return ensured;
  }
  std::error_code error;
  const auto root = fs::weakly_canonical(fs::absolute(game), error);
  if (error)
    return absl::InvalidArgumentError("Cannot resolve game work root: " + error.message());
  const auto work = fs::weakly_canonical(fs::absolute(requested), error);
  if (error)
    return absl::InvalidArgumentError("Cannot resolve requested work directory: " + error.message());
  const auto relative = work.lexically_relative(root);
  if (relative.empty() || *relative.begin() == "..")
    return requested;
  const auto ensured = EnsureStateDirectory(game);
  if (!ensured.ok())
    return ensured;
  if (*relative.begin() == kDirectory)
    return requested;
  return relative == "." ? StatePath(game, "work") : StatePath(game, "work") / relative;
}

absl::Status RegisterPublished(
    const fs::path& game,
    const fs::path& path,
    const std::string& producer,
    const std::string& run_id) {
  const fs::path relative = path.is_absolute() ? path.lexically_relative(fs::absolute(game)) : path;
  Entry published;
  published.path = relative.generic_string();
  published.role = "published";
  published.producer = producer;
  published.run_id = run_id;
  return Update(game, producer, &published);
}

absl::Status RegisterInput(const fs::path& game, const fs::path& path, const std::string& producer) {
  const fs::path relative = path.is_absolute() ? path.lexically_relative(fs::absolute(game)) : path;
  Entry input;
  input.path = relative.generic_string();
  input.role = "input-reference";
  input.producer = producer;
  return Update(game, producer, &input);
}
} // namespace hm::game_storage
