#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::stitching {

enum class TransactionJournalKind {
  kRink,
  kStitch,
};

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
    bool force_portable_fallback,
    size_t maximum_bytes,
    bool durable = true);

// Applies the shared size contract for config.yaml, s.png, and rink_mask_*.png
// before creating an independent rollback snapshot.
absl::Status snapshot_rink_artifact_for_rollback(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    bool force_portable_fallback);

// Returns the validated size of one no-follow rink rollback artifact.
absl::StatusOr<uint64_t> rink_rollback_artifact_size(const std::filesystem::path& source);

// Legacy roots are scanned once. New transactions durably publish a pending
// marker before creating a journal, allowing the steady-state recovery path to
// avoid enumerating the game directory.
absl::StatusOr<bool> transaction_recovery_scan_required(const std::filesystem::path& root, TransactionJournalKind kind);
absl::Status mark_transaction_recovery_pending(const std::filesystem::path& root, TransactionJournalKind kind);
absl::Status complete_transaction_recovery(const std::filesystem::path& root, TransactionJournalKind kind);

// Publishes a complete journal state through an fsynced temporary file and an
// atomic rename, then makes the renamed directory entry durable.
absl::Status publish_transaction_state(const std::filesystem::path& transaction, const std::string& contents);

} // namespace hm::stitching
