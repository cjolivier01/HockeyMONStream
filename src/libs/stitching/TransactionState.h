#pragma once

#include <cstddef>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::stitching {

class PinnedDirectory {
 public:
  PinnedDirectory() = default;
  ~PinnedDirectory();
  PinnedDirectory(PinnedDirectory&& other) noexcept;
  PinnedDirectory& operator=(PinnedDirectory&& other) noexcept;
  PinnedDirectory(const PinnedDirectory&) = delete;
  PinnedDirectory& operator=(const PinnedDirectory&) = delete;

  static absl::StatusOr<PinnedDirectory> Open(const std::filesystem::path& path, const std::string& description);
  absl::StatusOr<std::optional<PinnedDirectory>> OpenChild(const std::string& name, const std::string& description)
      const;

  std::filesystem::path path() const;
  int descriptor() const {
    return descriptor_;
  }

 private:
  explicit PinnedDirectory(int descriptor) : descriptor_(descriptor) {}
  int descriptor_{-1};
};

// Removes a pinned directory without traversing symlinks, but only while its
// parent entry still resolves to the inode that was originally opened.
absl::Status remove_pinned_directory(
    const PinnedDirectory& parent,
    const std::string& name,
    const PinnedDirectory& directory);

// Reads one bounded regular file through a nonblocking no-follow descriptor so
// validation and parsing consume the same inode.
absl::StatusOr<std::string> read_bounded_regular_file_no_follow(
    const std::filesystem::path& path,
    size_t maximum_bytes,
    const std::string& description);

// Snapshots one opened regular file into a rollback directory without following
// symlinks or trusting that the source pathname remains bound to the same inode.
absl::Status snapshot_regular_file_for_rollback(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    bool force_portable_fallback = false,
    size_t maximum_bytes = std::numeric_limits<size_t>::max());

// Publishes a complete journal state through an fsynced temporary file and an
// atomic rename, then makes the renamed directory entry durable.
absl::Status publish_transaction_state(const std::filesystem::path& transaction, const std::string& contents);

} // namespace hm::stitching
