#include "hstream/src/libs/stitching/TransactionState.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hm::stitching {
namespace {

namespace fs = std::filesystem;

absl::Status fsync_directory(const fs::path& directory) {
  const int descriptor = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
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

} // namespace

absl::Status snapshot_regular_file_for_rollback(
    const fs::path& source,
    const fs::path& destination,
    bool force_portable_fallback) {
  const fs::path temporary = destination.parent_path() / ("." + destination.filename().string() + ".hstream-partial");
  std::error_code error;
  fs::remove(temporary, error);
  if (error)
    return absl::InternalError("Unable to remove incomplete rollback artifact: " + error.message());
  const int source_fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
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
