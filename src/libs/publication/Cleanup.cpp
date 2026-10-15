#include "hstream/src/libs/publication/Cleanup.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include "hstream/src/libs/recording/Database.h"

namespace hm::publication {
namespace {
namespace fs = std::filesystem;
struct Fd {
  int value;
  explicit Fd(int fd) : value(fd) {}
  ~Fd() {
    close();
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  void close() {
    if (value >= 0)
      ::close(std::exchange(value, -1));
  }
};
[[noreturn]] void fail(const std::string& message) {
  throw std::runtime_error(message);
}
void check(bool success, const std::string& message) {
  if (!success)
    fail(message + ": " + std::strerror(errno));
}
void sync(int fd) {
  check(::fsync(fd) == 0, "Cannot synchronize cleanup directory");
}
bool inspect(int parent, const std::string& name, struct stat& result) {
  if (::fstatat(parent, name.c_str(), &result, AT_SYMLINK_NOFOLLOW) == 0)
    return true;
  if (errno != ENOENT)
    fail("Cannot inspect cleanup entry " + name + ": " + std::strerror(errno));
  return false;
}
bool matches(const struct stat& st, const YAML::Node& expected, bool contents = false) {
  return expected && uint64_t(st.st_dev) == expected["device"].as<uint64_t>() &&
      uint64_t(st.st_ino) == expected["inode"].as<uint64_t>() &&
      bool(S_ISDIR(st.st_mode)) == expected["directory"].as<bool>() &&
      (!contents ||
       (S_ISREG(st.st_mode) && int64_t(st.st_size) == expected["size"].as<int64_t>() &&
        int64_t(st.st_mtim.tv_sec) == expected["mtime"].as<int64_t>() &&
        int64_t(st.st_mtim.tv_nsec) == expected["mtime_ns"].as<int64_t>()));
}
YAML::Node directory_identity(const struct stat& st) {
  YAML::Node result;
  result["device"] = uint64_t(st.st_dev);
  result["inode"] = uint64_t(st.st_ino);
  result["directory"] = true;
  return result;
}
bool empty(int fd) {
  // A separate open file description avoids sharing the directory stream's
  // offset with the descriptor used for subsequent recovery checks.
  const int copy = ::openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  check(copy >= 0, "Cannot inspect cleanup directory contents");
  DIR* directory = ::fdopendir(copy);
  if (!directory) {
    ::close(copy);
    fail("Cannot inspect cleanup directory contents");
  }
  bool result = true;
  errno = 0;
  while (const auto* entry = ::readdir(directory)) {
    if (std::strcmp(entry->d_name, ".") && std::strcmp(entry->d_name, "..")) {
      result = false;
      break;
    }
  }
  const int error = errno;
  ::closedir(directory);
  if (error)
    fail("Cannot read cleanup directory: " + std::string(std::strerror(error)));
  return result;
}
void fault(const char* point) {
  const char* requested = ::getenv("HSTREAM_TEST_CLEANUP_STOP");
  if (requested && std::string(requested) == point)
    fail(std::string("Injected cleanup interruption: ") + point);
}
bool restore(int quarantine, int parent, const std::string& name, const struct stat& st) {
  // Never rename over a concurrently created public entry. NFS does not always
  // support RENAME_NOREPLACE; hard links provide the same no-replace contract
  // for regular files and symlinks. A directory is retained in quarantine if
  // the filesystem cannot safely restore its original name.
  struct stat restored{};
  if (inspect(parent, name, restored) && restored.st_dev == st.st_dev && restored.st_ino == st.st_ino) {
    // Recover a crash after the no-replace link was made durable but before
    // its private alias was retired.
    sync(parent);
    check(
        ::unlinkat(quarantine, "entry", S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0) == 0,
        "Cannot retire restored cleanup link");
    sync(quarantine);
    return true;
  }
  const bool force_fallback = ::getenv("HSTREAM_TEST_CLEANUP_NO_RENAME_NOREPLACE") != nullptr;
  if (!force_fallback && ::syscall(SYS_renameat2, quarantine, "entry", parent, name.c_str(), 1u) == 0) {
    sync(parent);
    sync(quarantine);
    return true;
  }
  const int error = force_fallback ? EOPNOTSUPP : errno;
  if ((error != EINVAL && error != ENOSYS && error != EOPNOTSUPP) || S_ISDIR(st.st_mode))
    return false;
  if (::linkat(quarantine, "entry", parent, name.c_str(), 0) != 0)
    return false;
  sync(parent);
  fault("after-restore-link");
  check(::unlinkat(quarantine, "entry", 0) == 0, "Cannot retire restored cleanup link");
  sync(quarantine);
  return true;
}
} // namespace

void ResumeCleanup(YAML::Node& run, const CleanupCheckpoint& checkpoint) {
  auto pending = run["cleanup_pending"];
  if (!pending)
    return;
  const fs::path source = pending["source"].as<std::string>();
  const fs::path quarantine = pending["quarantine"].as<std::string>();
  if (source.parent_path() != quarantine.parent_path() || source.filename().empty() ||
      quarantine.filename().string().rfind(".hstream-cleanup-", 0) != 0)
    fail("Invalid pending cleanup paths");
  const std::string name = source.filename();
  const std::string private_name = quarantine.filename();
  Fd parent(::open(source.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  check(parent.value >= 0, "Cannot open cleanup parent " + source.parent_path().string());
  if (!matches(
          [&] {
            struct stat st{};
            check(::fstat(parent.value, &st) == 0, "Cannot inspect cleanup parent");
            return st;
          }(),
          pending["parent_identity"]))
    fail("Cleanup parent directory was replaced: " + source.parent_path().string());

  auto phase = pending["phase"].as<std::string>();
  struct stat private_stat{};
  if (!inspect(parent.value, private_name, private_stat)) {
    if (phase == "removed") {
      run.remove("cleanup_pending");
      checkpoint();
      return;
    }
    if (phase != "reserved")
      fail("Cleanup quarantine disappeared: " + quarantine.string());
    check(::mkdirat(parent.value, private_name.c_str(), 0700) == 0, "Cannot create cleanup quarantine");
    sync(parent.value);
    fault("after-directory-create");
  }
  Fd directory(::openat(parent.value, private_name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  check(directory.value >= 0, "Cannot open cleanup quarantine");
  check(::fstat(directory.value, &private_stat) == 0, "Cannot inspect cleanup quarantine");
  if (!S_ISDIR(private_stat.st_mode) || private_stat.st_uid != ::geteuid() || (private_stat.st_mode & 0077))
    fail("Cleanup quarantine must be private and owned by the current user");
  if (phase == "reserved") {
    // The random name and creation intent were saved before mkdir. No source
    // entry is moved until this empty directory's identity is durable.
    if (!empty(directory.value))
      fail("Unrecognized cleanup quarantine contents");
    pending["quarantine_identity"] = directory_identity(private_stat);
    pending["phase"] = phase = "ready";
    checkpoint();
  } else if (!matches(private_stat, pending["quarantine_identity"])) {
    fail("Cleanup quarantine directory was replaced");
  }

  const auto finish = [&] {
    pending["phase"] = "removed";
    checkpoint();
    if (!empty(directory.value))
      fail("Unexpected entries remain in cleanup quarantine: " + quarantine.string());
    struct stat current{};
    if (!inspect(parent.value, private_name, current) || !matches(current, pending["quarantine_identity"]))
      fail("Cleanup quarantine directory was replaced");
    // Closing first avoids NFS retaining an open directory during retirement.
    directory.close();
    check(::unlinkat(parent.value, private_name.c_str(), AT_REMOVEDIR) == 0, "Cannot retire cleanup quarantine");
    sync(parent.value);
    fault("after-directory-remove");
    run.remove("cleanup_pending");
    checkpoint();
  };
  if (phase == "removed") {
    finish();
    return;
  }
  if (phase != "ready" && phase != "deleting")
    fail("Invalid cleanup recovery phase");

  struct stat entry{};
  if (!inspect(directory.value, "entry", entry)) {
    if (phase == "deleting") {
      // The deleting checkpoint precedes unlink; a missing entry after a crash
      // means deletion completed. Do not touch any replacement at source.
      finish();
      return;
    }
    if (!inspect(parent.value, name, entry)) {
      finish();
      return;
    }
    if (!matches(entry, pending["identity"], pending["contents"].as<bool>())) {
      finish();
      fail("Refusing to remove replaced file: " + source.string());
    }
    // The destination is inside our exclusively owned, empty quarantine. Plain
    // rename is atomic on NFS too; replacing source here is caught below.
    check(::renameat(parent.value, name.c_str(), directory.value, "entry") == 0, "Cannot quarantine cleanup entry");
    sync(directory.value);
    sync(parent.value);
    fault("after-quarantine");
    check(inspect(directory.value, "entry", entry), "Cannot inspect quarantined cleanup entry");
  }
  if (!matches(entry, pending["identity"], pending["contents"].as<bool>())) {
    if (restore(directory.value, parent.value, name, entry)) {
      finish();
      fail("Refusing to remove replaced file: " + source.string());
    }
    fail("Replaced file preserved at " + (quarantine / "entry").string() + "; cannot restore without overwriting");
  }
  pending["phase"] = "deleting";
  checkpoint();
  if (::unlinkat(directory.value, "entry", S_ISDIR(entry.st_mode) ? AT_REMOVEDIR : 0) != 0) {
    const int error = errno;
    if (restore(directory.value, parent.value, name, entry)) {
      finish();
      fail("Cannot clean " + source.string() + ": " + std::strerror(error));
    }
    fail("Cannot clean " + source.string() + "; entry preserved at " + (quarantine / "entry").string());
  }
  sync(directory.value);
  fault("after-delete");
  finish();
}

void CleanupOwned(
    const fs::path& path,
    const YAML::Node& expected,
    bool contents,
    YAML::Node& run,
    const CleanupCheckpoint& checkpoint) {
  ResumeCleanup(run, checkpoint);
  const auto absolute = fs::absolute(path).lexically_normal();
  struct stat entry{};
  if (!inspect(AT_FDCWD, absolute.string(), entry))
    return;
  // Explicit archive directories may be symlinks. Resolve the parent once,
  // then pin and record that actual directory for the whole operation.
  const auto source = fs::canonical(absolute.parent_path()) / absolute.filename();
  if (!matches(entry, expected, contents))
    fail("Refusing to remove replaced file: " + source.string());
  if (S_ISDIR(entry.st_mode)) {
    Fd directory(::open(source.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    check(directory.value >= 0, "Cannot inspect cleanup directory");
    if (!empty(directory.value))
      fail("Cannot clean nonempty directory: " + source.string());
  }
  Fd parent(::open(source.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  check(parent.value >= 0, "Cannot open cleanup parent");
  struct stat parent_stat{};
  check(::fstat(parent.value, &parent_stat) == 0, "Cannot inspect cleanup parent");
  YAML::Node pending;
  pending["source"] = source.string();
  pending["identity"] = YAML::Clone(expected);
  pending["contents"] = contents;
  pending["parent_identity"] = directory_identity(parent_stat);
  pending["quarantine"] = (source.parent_path() / (".hstream-cleanup-" + hm::recording::NewGuid())).string();
  pending["phase"] = "reserved";
  run["cleanup_pending"] = pending;
  checkpoint();
  ResumeCleanup(run, checkpoint);
}
} // namespace hm::publication
