#include "hstream/src/libs/stitching/TransactionState.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
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

absl::Status publish_transaction_state(const fs::path& transaction, const std::string& contents) {
  if (contents != "PREPARED\n" && contents != "BACKING_UP\n" && contents != "BACKED_UP\n" &&
      contents != "ROLLING_BACK\n" && contents != "COMMITTED\n" && contents != "ROLLED_BACK\n") {
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
