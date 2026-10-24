#include "hstream/src/libs/program/ProgramFileOutput.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace hm::program {
namespace {
absl::Status Error(const char* operation) {
  const auto code = errno;
  const auto detail = std::string(operation) + ": " + std::strerror(code);
  return code == EEXIST ? absl::AlreadyExistsError(detail) : absl::UnavailableError(detail);
}
} // namespace
struct ProgramFileOutput::Impl {
  int directory{-1}, file{-1};
  std::string name;
  bool committed{false};
  ~Impl() {
    if (file >= 0)
      close(file);
    if (directory >= 0)
      close(directory);
  }
};
ProgramFileOutput::ProgramFileOutput(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ProgramFileOutput::~ProgramFileOutput() = default;
absl::StatusOr<std::unique_ptr<ProgramFileOutput>> ProgramFileOutput::Create(const std::filesystem::path& destination) {
  if (destination.empty() || destination.filename().empty() || destination.filename() == "." ||
      destination.filename() == "..")
    return absl::InvalidArgumentError("Program output requires a filename");
  auto impl = std::make_unique<Impl>();
  const auto parent = destination.has_parent_path() ? destination.parent_path() : std::filesystem::path(".");
  impl->directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (impl->directory < 0)
    return Error("Cannot open Program output directory");
  impl->name = destination.filename().string();
  struct stat entry {};
  if (!fstatat(impl->directory, impl->name.c_str(), &entry, AT_SYMLINK_NOFOLLOW))
    return absl::AlreadyExistsError("Program output already exists");
  if (errno != ENOENT)
    return Error("Cannot inspect Program output destination");
  // An unnamed inode is removed by the kernel even on SIGKILL. Never fall back
  // to an externally visible partial MP4 on filesystems lacking O_TMPFILE.
  impl->file = openat(impl->directory, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
  if (impl->file < 0)
    return Error("Program output requires a local filesystem supporting O_TMPFILE");
  return std::unique_ptr<ProgramFileOutput>(new ProgramFileOutput(std::move(impl)));
}
int ProgramFileOutput::fd() const {
  return impl_->file;
}
absl::Status ProgramFileOutput::Commit(const std::function<bool()>& cancelled) {
  if (impl_->committed)
    return absl::FailedPreconditionError("Program output is already published");
  if (cancelled && cancelled())
    return absl::CancelledError("Program cancelled before publication");
  if (fsync(impl_->file))
    return Error("Cannot flush completed Program output");
  if (cancelled && cancelled())
    return absl::CancelledError("Program cancelled while flushing output");
  // /proc/self/fd allows an unprivileged owner to link its O_TMPFILE inode.
  const auto descriptor = "/proc/self/fd/" + std::to_string(impl_->file);
  if (linkat(AT_FDCWD, descriptor.c_str(), impl_->directory, impl_->name.c_str(), AT_SYMLINK_FOLLOW))
    return Error("Cannot publish completed Program output");
  impl_->committed = true;
  if (fsync(impl_->directory))
    return Error("Program output is published but directory flush failed");
  return absl::OkStatus();
}
} // namespace hm::program
