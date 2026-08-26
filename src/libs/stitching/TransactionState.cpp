#include "hstream/src/libs/stitching/TransactionState.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
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

constexpr size_t kMaximumRinkConfigRollbackBytes = 16ULL * 1024ULL * 1024ULL;
constexpr size_t kMaximumRinkMaskRollbackBytes = 128ULL * 1024ULL * 1024ULL;
constexpr size_t kMaximumStitchedSnapshotRollbackBytes = 512ULL * 1024ULL * 1024ULL;

struct RecoveryMarkerNames {
  const char* protocol;
  const char* pending;
};

RecoveryMarkerNames recovery_marker_names(TransactionJournalKind kind) {
  switch (kind) {
    case TransactionJournalKind::kRink:
      return {".hstream-rink-journal-v1", ".hstream-rink-recovery-pending"};
    case TransactionJournalKind::kStitch:
      return {".hstream-stitch-journal-v1", ".hstream-stitch-recovery-pending"};
  }
  return {nullptr, nullptr};
}

absl::StatusOr<bool> regular_empty_marker_exists(int root_descriptor, const char* name) {
  struct stat metadata{};
  if (::fstatat(root_descriptor, name, &metadata, AT_SYMLINK_NOFOLLOW) == 0) {
    if (!S_ISREG(metadata.st_mode) || metadata.st_size != 0)
      return absl::FailedPreconditionError("Invalid transaction recovery marker: " + std::string(name));
    return true;
  }
  if (errno == ENOENT)
    return false;
  return absl::InternalError(
      "Unable to inspect transaction recovery marker " + std::string(name) + ": " + std::strerror(errno));
}

absl::Status ensure_empty_marker(int root_descriptor, const char* name) {
  auto exists = regular_empty_marker_exists(root_descriptor, name);
  if (!exists.ok())
    return exists.status();
  if (*exists)
    return absl::OkStatus();

  static std::atomic<uint64_t> temporary_sequence{0};
  const std::string temporary = "." + std::string(name) + ".tmp-" + std::to_string(::getpid()) + "-" +
      std::to_string(temporary_sequence.fetch_add(1, std::memory_order_relaxed));
  const int descriptor =
      ::openat(root_descriptor, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (descriptor < 0)
    return absl::InternalError("Unable to create transaction recovery marker: " + std::string(std::strerror(errno)));
  struct TemporaryCleanup {
    int root_descriptor;
    int descriptor;
    std::string name;
    ~TemporaryCleanup() {
      if (descriptor >= 0)
        ::close(descriptor);
      if (!name.empty())
        ::unlinkat(root_descriptor, name.c_str(), 0);
    }
  } cleanup{root_descriptor, descriptor, temporary};
  if (::fsync(descriptor) != 0)
    return absl::InternalError("Unable to sync transaction recovery marker: " + std::string(std::strerror(errno)));
  if (::close(descriptor) != 0) {
    cleanup.descriptor = -1;
    return absl::InternalError("Unable to close transaction recovery marker: " + std::string(std::strerror(errno)));
  }
  cleanup.descriptor = -1;
  if (::linkat(root_descriptor, temporary.c_str(), root_descriptor, name, 0) != 0) {
    if (errno != EEXIST)
      return absl::InternalError("Unable to publish transaction recovery marker: " + std::string(std::strerror(errno)));
    auto raced = regular_empty_marker_exists(root_descriptor, name);
    if (!raced.ok() || !*raced)
      return raced.ok() ? absl::FailedPreconditionError("Invalid transaction recovery marker") : raced.status();
  }
  if (::unlinkat(root_descriptor, temporary.c_str(), 0) != 0)
    return absl::InternalError("Unable to remove temporary recovery marker: " + std::string(std::strerror(errno)));
  cleanup.name.clear();
  if (::fsync(root_descriptor) != 0)
    return absl::InternalError("Unable to sync transaction recovery marker: " + std::string(std::strerror(errno)));
  return absl::OkStatus();
}

absl::Status clear_empty_marker(int root_descriptor, const char* name) {
  auto exists = regular_empty_marker_exists(root_descriptor, name);
  if (!exists.ok())
    return exists.status();
  if (!*exists)
    return absl::OkStatus();
  if (::unlinkat(root_descriptor, name, 0) != 0)
    return absl::InternalError("Unable to clear transaction recovery marker: " + std::string(std::strerror(errno)));
  if (::fsync(root_descriptor) != 0)
    return absl::InternalError(
        "Unable to sync cleared transaction recovery marker: " + std::string(std::strerror(errno)));
  return absl::OkStatus();
}

absl::StatusOr<int> open_recovery_root(const fs::path& root) {
  const int descriptor = ::open(root.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NONBLOCK);
  if (descriptor < 0)
    return absl::FailedPreconditionError(
        "Unable to open transaction recovery root: " + std::string(std::strerror(errno)));
  return descriptor;
}

bool is_rink_mask_rollback_name(const std::string& name) {
  constexpr std::string_view prefix = "rink_mask_";
  constexpr std::string_view suffix = ".png";
  if (name.size() <= prefix.size() + suffix.size() || name.compare(0, prefix.size(), prefix) != 0 ||
      name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return false;
  }
  const std::string_view index(name.data() + prefix.size(), name.size() - prefix.size() - suffix.size());
  return (index.size() == 1 || index.front() != '0') &&
      std::all_of(index.begin(), index.end(), [](unsigned char value) { return std::isdigit(value); });
}

size_t maximum_rink_rollback_bytes(const fs::path& source) {
  const std::string name = source.filename().string();
  if (name == "config.yaml")
    return kMaximumRinkConfigRollbackBytes;
  if (name == "s.png")
    return kMaximumStitchedSnapshotRollbackBytes;
  if (is_rink_mask_rollback_name(name))
    return kMaximumRinkMaskRollbackBytes;
  return 0;
}

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
  // The caller-selected game directory may itself be a symlink. Follow that
  // boundary once and pin the resolved directory; children are opened no-follow.
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NONBLOCK);
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
    bool force_portable_fallback,
    size_t maximum_bytes,
    bool durable) {
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
  if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0 || static_cast<uint64_t>(metadata.st_size) > maximum_bytes)
    return absl::FailedPreconditionError("Rollback source is invalid or oversized: " + source.string());

  const int destination_fd =
      ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, metadata.st_mode & 0777);
  if (destination_fd < 0) {
    return absl::InternalError(
        "Unable to create rollback artifact " + temporary.string() + ": " + std::strerror(errno));
  }
  DescriptorCleanup destination_descriptor_cleanup{destination_fd};
  if (const char* delay = std::getenv("HM_TEST_ROLLBACK_PRE_COPY_DELAY_MS")) {
    const uint64_t delay_ms = std::strtoull(delay, nullptr, 10);
    if (delay_ms > 0)
      ::usleep(std::min<uint64_t>(delay_ms, 10'000) * 1000);
  }
  const bool cloned = !force_portable_fallback && ::ioctl(destination_fd, FICLONE, source_fd) == 0;
  const int clone_error = force_portable_fallback ? EOPNOTSUPP : errno;
  if (!cloned) {
    if (::lseek(source_fd, 0, SEEK_SET) < 0 || ::ftruncate(destination_fd, 0) != 0 ||
        ::lseek(destination_fd, 0, SEEK_SET) < 0) {
      return absl::InternalError(
          "Unable to reset rollback files for portable copy: " + std::string(std::strerror(errno)));
    }
    std::array<unsigned char, 64 * 1024> buffer{};
    uint64_t remaining = static_cast<uint64_t>(metadata.st_size);
    while (remaining > 0) {
      const size_t requested = static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining));
      const ssize_t count = ::read(source_fd, buffer.data(), requested);
      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0) {
        return absl::InternalError(
            "Unable to read rollback source after reflink (" + std::string(std::strerror(clone_error)) +
            ") failed: " + std::string(std::strerror(errno)));
      }
      if (count == 0)
        return absl::AbortedError("Rollback source was truncated while creating an independent snapshot");
      size_t written = 0;
      while (written < static_cast<size_t>(count)) {
        const ssize_t result = ::write(destination_fd, buffer.data() + written, static_cast<size_t>(count) - written);
        if (result < 0 && errno == EINTR)
          continue;
        if (result <= 0) {
          return absl::InternalError(
              "Unable to write rollback artifact after reflink (" + std::string(std::strerror(clone_error)) +
              ") failed: " + std::string(std::strerror(errno)));
        }
        written += static_cast<size_t>(result);
      }
      remaining -= static_cast<uint64_t>(count);
    }
  }

  struct stat source_after{};
  struct stat destination_metadata{};
  if (::fstat(source_fd, &source_after) != 0 || ::fstat(destination_fd, &destination_metadata) != 0 ||
      source_after.st_dev != metadata.st_dev || source_after.st_ino != metadata.st_ino ||
      source_after.st_size != metadata.st_size || source_after.st_mtim.tv_sec != metadata.st_mtim.tv_sec ||
      source_after.st_mtim.tv_nsec != metadata.st_mtim.tv_nsec ||
      source_after.st_ctim.tv_sec != metadata.st_ctim.tv_sec ||
      source_after.st_ctim.tv_nsec != metadata.st_ctim.tv_nsec || destination_metadata.st_size != metadata.st_size) {
    return absl::AbortedError("Rollback source changed while creating an independent snapshot");
  }

  timespec times[2] = {};
  times[0].tv_nsec = UTIME_OMIT;
  times[1] = metadata.st_mtim;
  const int mode_result = ::fchmod(destination_fd, metadata.st_mode & 07777);
  const bool mode_restored = mode_result == 0 || errno == EOPNOTSUPP || errno == ENOTSUP || errno == EPERM;
  if (!mode_restored || ::futimens(destination_fd, times) != 0 || (durable && ::fsync(destination_fd) != 0)) {
    return absl::InternalError("Unable to preserve rollback artifact metadata: " + std::string(std::strerror(errno)));
  }
  destination_descriptor_cleanup.descriptor = -1;
  if (::close(destination_fd) != 0)
    return absl::InternalError("Unable to close rollback artifact: " + std::string(std::strerror(errno)));
  return publish();
}

absl::Status snapshot_rink_artifact_for_rollback(
    const fs::path& source,
    const fs::path& destination,
    bool force_portable_fallback) {
  const size_t maximum_bytes = maximum_rink_rollback_bytes(source);
  if (maximum_bytes == 0)
    return absl::FailedPreconditionError("Unrecognized rink rollback artifact: " + source.string());
  return snapshot_regular_file_for_rollback(source, destination, force_portable_fallback, maximum_bytes);
}

absl::StatusOr<uint64_t> rink_rollback_artifact_size(const fs::path& source) {
  const size_t maximum_bytes = maximum_rink_rollback_bytes(source);
  if (maximum_bytes == 0)
    return absl::FailedPreconditionError("Unrecognized rink rollback artifact: " + source.string());
  const int descriptor = ::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0)
    return absl::FailedPreconditionError("Unable to open rink rollback artifact: " + source.string());
  struct DescriptorCleanup {
    int descriptor;
    ~DescriptorCleanup() {
      ::close(descriptor);
    }
  } cleanup{descriptor};
  struct stat metadata{};
  if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
      static_cast<uint64_t>(metadata.st_size) > maximum_bytes) {
    return absl::FailedPreconditionError("Rink rollback artifact is invalid or oversized: " + source.string());
  }
  return static_cast<uint64_t>(metadata.st_size);
}

absl::StatusOr<bool> transaction_recovery_scan_required(const fs::path& root, TransactionJournalKind kind) {
  auto opened = open_recovery_root(root);
  if (!opened.ok())
    return opened.status();
  const int descriptor = *opened;
  struct DescriptorCleanup {
    int descriptor;
    ~DescriptorCleanup() {
      ::close(descriptor);
    }
  } cleanup{descriptor};
  const RecoveryMarkerNames names = recovery_marker_names(kind);
  auto protocol = regular_empty_marker_exists(descriptor, names.protocol);
  if (!protocol.ok())
    return protocol.status();
  auto pending = regular_empty_marker_exists(descriptor, names.pending);
  if (!pending.ok())
    return pending.status();
  const char* force = std::getenv("HM_TEST_FORCE_TRANSACTION_RECOVERY_SCAN");
  return (force != nullptr && std::strcmp(force, "1") == 0) || !*protocol || *pending;
}

absl::Status mark_transaction_recovery_pending(const fs::path& root, TransactionJournalKind kind) {
  auto opened = open_recovery_root(root);
  if (!opened.ok())
    return opened.status();
  const int descriptor = *opened;
  struct DescriptorCleanup {
    int descriptor;
    ~DescriptorCleanup() {
      ::close(descriptor);
    }
  } cleanup{descriptor};
  const RecoveryMarkerNames names = recovery_marker_names(kind);
  auto status = ensure_empty_marker(descriptor, names.protocol);
  if (!status.ok())
    return status;
  return ensure_empty_marker(descriptor, names.pending);
}

absl::Status complete_transaction_recovery(const fs::path& root, TransactionJournalKind kind) {
  auto opened = open_recovery_root(root);
  if (!opened.ok())
    return opened.status();
  const int descriptor = *opened;
  struct DescriptorCleanup {
    int descriptor;
    ~DescriptorCleanup() {
      ::close(descriptor);
    }
  } cleanup{descriptor};
  const RecoveryMarkerNames names = recovery_marker_names(kind);
  auto status = ensure_empty_marker(descriptor, names.protocol);
  if (!status.ok())
    return status;
  return clear_empty_marker(descriptor, names.pending);
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
