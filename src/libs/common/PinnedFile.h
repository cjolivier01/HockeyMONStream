#pragma once

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <string>

namespace hm {

// Publish the inode held by an open descriptor, never a mutable source path.
// Older kernels require CAP_DAC_READ_SEARCH for AT_EMPTY_PATH. The documented
// procfs alternative supports ordinary users while retaining descriptor identity
// and linkat's no-replace behavior. An unlinked inode still cannot be resurrected.
inline int link_pinned_file(int source_fd, int destination_dir_fd, const char* destination) {
  if (::linkat(source_fd, "", destination_dir_fd, destination, AT_EMPTY_PATH) == 0)
    return 0;
  if (errno != ENOENT && errno != EPERM && errno != EINVAL && errno != ENOSYS)
    return -1;
  const std::string pinned_path = "/proc/self/fd/" + std::to_string(source_fd);
  return ::linkat(AT_FDCWD, pinned_path.c_str(), destination_dir_fd, destination, AT_SYMLINK_FOLLOW);
}

} // namespace hm
