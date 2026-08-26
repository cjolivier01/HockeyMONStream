#include "hstream/src/libs/stitching/TransactionState.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hm::stitching {
namespace {

namespace fs = std::filesystem;

absl::Status fsync_directory(const fs::path& directory) {
  const std::string filename = directory.filename().string();
  const bool pinned_self_descriptor = directory.parent_path() == fs::path("/proc/self/fd") && !filename.empty() &&
      std::all_of(filename.begin(), filename.end(), [](unsigned char value) { return std::isdigit(value); });
  const int flags = O_RDONLY | O_CLOEXEC | O_DIRECTORY | (pinned_self_descriptor ? 0 : O_NOFOLLOW);
  const int descriptor = ::open(directory.c_str(), flags);
  if (descriptor < 0) {
    return absl::InternalError(
        "Unable to open transaction directory for fsync: " + directory.string() + ": " + std::strerror(errno));
  }
  const int result = ::fsync(descriptor);
  const int saved_errno = errno;
  ::close(descriptor);
  if (result != 0) {
    return absl::InternalError(
        "Unable to fsync transaction directory " + directory.string() + ": " + std::strerror(saved_errno));
  }
  return absl::OkStatus();
}

bool same_inode(const struct stat& first, const struct stat& second) {
  return first.st_dev == second.st_dev && first.st_ino == second.st_ino;
}

bool same_file_snapshot(const struct stat& first, const struct stat& second) {
  return same_inode(first, second) && first.st_mode == second.st_mode && first.st_size == second.st_size &&
      first.st_mtim.tv_sec == second.st_mtim.tv_sec && first.st_mtim.tv_nsec == second.st_mtim.tv_nsec &&
      first.st_ctim.tv_sec == second.st_ctim.tv_sec && first.st_ctim.tv_nsec == second.st_ctim.tv_nsec;
}

absl::Status verify_directory_binding(int parent_descriptor, const std::string& name, int directory_descriptor) {
  struct stat expected{};
  struct stat current{};
  if (::fstat(directory_descriptor, &expected) != 0 || !S_ISDIR(expected.st_mode))
    return absl::InternalError("Unable to inspect pinned transaction directory: " + std::string(std::strerror(errno)));
  if (::fstatat(parent_descriptor, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0) {
    return absl::AbortedError(
        "Transaction directory binding changed during recovery: " + std::string(std::strerror(errno)));
  }
  if (!S_ISDIR(current.st_mode) || !same_inode(expected, current))
    return absl::AbortedError("Transaction directory binding changed during recovery: " + name);
  return absl::OkStatus();
}

absl::Status remove_directory_contents_no_follow(int directory_descriptor) {
  const int iterator_descriptor = ::dup(directory_descriptor);
  if (iterator_descriptor < 0)
    return absl::InternalError(
        "Unable to duplicate transaction directory descriptor: " + std::string(std::strerror(errno)));
  DIR* iterator = ::fdopendir(iterator_descriptor);
  if (iterator == nullptr) {
    const int saved_errno = errno;
    ::close(iterator_descriptor);
    return absl::InternalError("Unable to enumerate transaction directory: " + std::string(std::strerror(saved_errno)));
  }
  struct IteratorCleanup {
    DIR* iterator;
    ~IteratorCleanup() {
      ::closedir(iterator);
    }
  } cleanup{iterator};

  while (true) {
    errno = 0;
    dirent* entry = ::readdir(iterator);
    if (entry == nullptr) {
      if (errno != 0)
        return absl::InternalError("Unable to enumerate transaction directory: " + std::string(std::strerror(errno)));
      break;
    }
    const std::string name(entry->d_name);
    if (name == "." || name == "..")
      continue;
    struct stat metadata{};
    if (::fstatat(directory_descriptor, name.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
      return absl::AbortedError("Transaction entry changed during cleanup: " + name + ": " + std::strerror(errno));
    }
    if (S_ISDIR(metadata.st_mode)) {
      const int child_descriptor =
          ::openat(directory_descriptor, name.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK);
      if (child_descriptor < 0)
        return absl::AbortedError("Transaction directory changed during cleanup: " + name);
      struct ChildCleanup {
        int descriptor;
        ~ChildCleanup() {
          ::close(descriptor);
        }
      } child_cleanup{child_descriptor};
      struct stat opened_metadata{};
      if (::fstat(child_descriptor, &opened_metadata) != 0 || !same_inode(metadata, opened_metadata))
        return absl::AbortedError("Transaction directory changed during cleanup: " + name);
      auto status = remove_directory_contents_no_follow(child_descriptor);
      if (!status.ok())
        return status;
      status = verify_directory_binding(directory_descriptor, name, child_descriptor);
      if (!status.ok())
        return status;
      if (::unlinkat(directory_descriptor, name.c_str(), AT_REMOVEDIR) != 0) {
        return absl::InternalError("Unable to remove transaction directory " + name + ": " + std::strerror(errno));
      }
    } else if (::unlinkat(directory_descriptor, name.c_str(), 0) != 0) {
      return absl::InternalError("Unable to remove transaction entry " + name + ": " + std::strerror(errno));
    }
  }
  if (::fsync(directory_descriptor) != 0)
    return absl::InternalError("Unable to sync cleaned transaction directory: " + std::string(std::strerror(errno)));
  return absl::OkStatus();
}

} // namespace

PinnedDirectory::~PinnedDirectory() {
  if (descriptor_ >= 0)
    ::close(descriptor_);
}

PinnedDirectory::PinnedDirectory(PinnedDirectory&& other) noexcept : descriptor_(other.descriptor_) {
  other.descriptor_ = -1;
}

PinnedDirectory& PinnedDirectory::operator=(PinnedDirectory&& other) noexcept {
  if (this == &other)
    return *this;
  if (descriptor_ >= 0)
    ::close(descriptor_);
  descriptor_ = other.descriptor_;
  other.descriptor_ = -1;
  return *this;
}

absl::StatusOr<PinnedDirectory> PinnedDirectory::Open(const fs::path& path, const std::string& description) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0)
    return absl::FailedPreconditionError("Unable to open " + description + ": " + std::strerror(errno));
  return PinnedDirectory(descriptor);
}

absl::StatusOr<std::optional<PinnedDirectory>> PinnedDirectory::OpenChild(
    const std::string& name,
    const std::string& description) const {
  if (descriptor_ < 0 || fs::path(name).filename() != name || name == "." || name == "..")
    return absl::InvalidArgumentError("Invalid child directory name: " + name);
  const int descriptor =
      ::openat(descriptor_, name.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0) {
    if (errno == ENOENT)
      return std::optional<PinnedDirectory>();
    return absl::FailedPreconditionError("Unable to open " + description + ": " + std::strerror(errno));
  }
  return std::optional<PinnedDirectory>(PinnedDirectory(descriptor));
}

fs::path PinnedDirectory::path() const {
  return fs::path("/proc/self/fd") / std::to_string(descriptor_);
}

absl::Status remove_pinned_directory(
    const PinnedDirectory& parent,
    const std::string& name,
    const PinnedDirectory& directory) {
  auto status = verify_directory_binding(parent.descriptor(), name, directory.descriptor());
  if (!status.ok())
    return status;
  status = remove_directory_contents_no_follow(directory.descriptor());
  if (!status.ok())
    return status;
  status = verify_directory_binding(parent.descriptor(), name, directory.descriptor());
  if (!status.ok())
    return status;
  if (::unlinkat(parent.descriptor(), name.c_str(), AT_REMOVEDIR) != 0)
    return absl::InternalError("Unable to remove recovered transaction " + name + ": " + std::strerror(errno));
  if (::fsync(parent.descriptor()) != 0)
    return absl::InternalError("Unable to sync recovered transaction removal: " + std::string(std::strerror(errno)));
  return absl::OkStatus();
}

absl::StatusOr<std::string> read_bounded_regular_file_no_follow(
    const fs::path& path,
    size_t maximum_bytes,
    const std::string& description) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0)
    return absl::FailedPreconditionError("Unable to open " + description + ": " + std::strerror(errno));
  struct DescriptorCleanup {
    int descriptor;
    ~DescriptorCleanup() {
      ::close(descriptor);
    }
  } cleanup{descriptor};
  struct stat metadata{};
  if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
      static_cast<uint64_t>(metadata.st_size) > maximum_bytes) {
    return absl::FailedPreconditionError("Invalid " + description);
  }
  std::string contents(static_cast<size_t>(metadata.st_size), '\0');
  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t count = ::read(descriptor, contents.data() + offset, contents.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return absl::FailedPreconditionError("Unable to read " + description);
    offset += static_cast<size_t>(count);
  }
  struct stat verified_metadata{};
  if (::fstat(descriptor, &verified_metadata) != 0 || !same_file_snapshot(metadata, verified_metadata))
    return absl::AbortedError(description + " changed while it was being read");
  return contents;
}

absl::Status snapshot_regular_file_for_rollback(
    const fs::path& source,
    const fs::path& destination,
    bool force_portable_fallback) {
  const fs::path temporary = destination.parent_path() / ("." + destination.filename().string() + ".hstream-partial");
  std::error_code error;
  fs::remove(temporary, error);
  if (error)
    return absl::InternalError("Unable to remove incomplete rollback artifact: " + error.message());
  const int source_fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (source_fd < 0) {
    if (errno == ELOOP)
      return absl::FailedPreconditionError("Rollback source is a symbolic link: " + source.string());
    return absl::InternalError("Unable to open rollback source " + source.string() + ": " + std::strerror(errno));
  }
  struct DescriptorCleanup {
    int descriptor;
    ~DescriptorCleanup() {
      if (descriptor >= 0)
        ::close(descriptor);
    }
  } source_cleanup{source_fd};
  struct DestinationCleanup {
    fs::path path;
    ~DestinationCleanup() {
      if (path.empty())
        return;
      std::error_code ignored;
      fs::remove(path, ignored);
    }
  } destination_cleanup{temporary};
  const auto publish = [&]() -> absl::Status {
    std::error_code rename_error;
    fs::rename(temporary, destination, rename_error);
    if (rename_error)
      return absl::InternalError("Unable to atomically publish rollback artifact: " + rename_error.message());
    destination_cleanup.path.clear();
    return absl::OkStatus();
  };

  struct stat metadata{};
  if (::fstat(source_fd, &metadata) != 0) {
    return absl::InternalError(
        "Unable to inspect opened rollback source " + source.string() + ": " + std::strerror(errno));
  }
  if (!S_ISREG(metadata.st_mode))
    return absl::FailedPreconditionError("Rollback source is not a regular file: " + source.string());

  int link_error = EOPNOTSUPP;
  if (!force_portable_fallback) {
    if (::link(source.c_str(), temporary.c_str()) == 0) {
      struct stat linked_metadata{};
      if (::lstat(temporary.c_str(), &linked_metadata) == 0 && S_ISREG(linked_metadata.st_mode) &&
          linked_metadata.st_dev == metadata.st_dev && linked_metadata.st_ino == metadata.st_ino) {
        if (::fsync(source_fd) != 0)
          return absl::InternalError("Unable to sync linked rollback artifact: " + std::string(std::strerror(errno)));
        return publish();
      }
      link_error = ESTALE;
      fs::remove(temporary, error);
      if (error)
        return absl::InternalError("Unable to remove invalid linked rollback artifact: " + error.message());
    } else {
      link_error = errno;
    }
  }

  const int destination_fd =
      ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, metadata.st_mode & 0777);
  if (destination_fd < 0) {
    return absl::InternalError(
        "Unable to create rollback artifact " + temporary.string() + ": " + std::strerror(errno));
  }
  DescriptorCleanup destination_descriptor_cleanup{destination_fd};
  const bool cloned = !force_portable_fallback && ::ioctl(destination_fd, FICLONE, source_fd) == 0;
  const int clone_error = force_portable_fallback ? EOPNOTSUPP : errno;
  if (!cloned) {
    if (::lseek(source_fd, 0, SEEK_SET) < 0 || ::ftruncate(destination_fd, 0) != 0 ||
        ::lseek(destination_fd, 0, SEEK_SET) < 0) {
      return absl::InternalError(
          "Unable to reset rollback files for portable copy: " + std::string(std::strerror(errno)));
    }
    std::array<unsigned char, 64 * 1024> buffer{};
    while (true) {
      const ssize_t count = ::read(source_fd, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0) {
        return absl::InternalError(
            "Unable to read rollback source after hard link (" + std::string(std::strerror(link_error)) +
            ") and reflink (" + std::string(std::strerror(clone_error)) +
            ") failed: " + std::string(std::strerror(errno)));
      }
      if (count == 0)
        break;
      size_t written = 0;
      while (written < static_cast<size_t>(count)) {
        const ssize_t result = ::write(destination_fd, buffer.data() + written, static_cast<size_t>(count) - written);
        if (result < 0 && errno == EINTR)
          continue;
        if (result <= 0) {
          return absl::InternalError(
              "Unable to write rollback artifact after hard link (" + std::string(std::strerror(link_error)) +
              ") and reflink (" + std::string(std::strerror(clone_error)) +
              ") failed: " + std::string(std::strerror(errno)));
        }
        written += static_cast<size_t>(result);
      }
    }
  }

  timespec times[2] = {};
  times[0].tv_nsec = UTIME_OMIT;
  times[1] = metadata.st_mtim;
  const int mode_result = ::fchmod(destination_fd, metadata.st_mode & 07777);
  const bool mode_restored = mode_result == 0 || errno == EOPNOTSUPP || errno == ENOTSUP || errno == EPERM;
  if (!mode_restored || ::futimens(destination_fd, times) != 0 || ::fsync(destination_fd) != 0) {
    return absl::InternalError("Unable to preserve rollback artifact metadata: " + std::string(std::strerror(errno)));
  }
  destination_descriptor_cleanup.descriptor = -1;
  if (::close(destination_fd) != 0)
    return absl::InternalError("Unable to close rollback artifact: " + std::string(std::strerror(errno)));
  return publish();
}

absl::Status publish_transaction_state(const fs::path& transaction, const std::string& contents) {
  if (contents != "PREPARED\n" && contents != "BACKING_UP\n" && contents != "BACKED_UP\n" &&
      contents != "LEGACY_MIGRATE\n" && contents != "ROLLING_BACK\n" && contents != "COMMITTED\n" &&
      contents != "RESTORED\n" && contents != "ROLLED_BACK\n") {
    return absl::InvalidArgumentError("Invalid transaction state contents");
  }

  std::string pattern = (transaction / ".state-XXXXXX").string();
  std::vector<char> writable_pattern(pattern.begin(), pattern.end());
  writable_pattern.push_back('\0');
  const int descriptor = ::mkstemp(writable_pattern.data());
  if (descriptor < 0) {
    return absl::InternalError("Unable to create temporary transaction state: " + std::string(std::strerror(errno)));
  }
  const fs::path temporary(writable_pattern.data());
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      if (path.empty())
        return;
      std::error_code ignored;
      fs::remove(path, ignored);
    }
  } cleanup{temporary};

  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t written = ::write(descriptor, contents.data() + offset, contents.size() - offset);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      const int saved_errno = errno;
      ::close(descriptor);
      return absl::InternalError("Unable to write transaction state: " + std::string(std::strerror(saved_errno)));
    }
    offset += static_cast<size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    const int saved_errno = errno;
    ::close(descriptor);
    return absl::InternalError("Unable to fsync transaction state: " + std::string(std::strerror(saved_errno)));
  }
  if (::close(descriptor) != 0) {
    return absl::InternalError("Unable to close transaction state: " + std::string(std::strerror(errno)));
  }

  std::error_code error;
  fs::rename(temporary, transaction / "state", error);
  if (error)
    return absl::InternalError("Unable to atomically publish transaction state: " + error.message());
  cleanup.path.clear();
  return fsync_directory(transaction);
}

} // namespace hm::stitching
