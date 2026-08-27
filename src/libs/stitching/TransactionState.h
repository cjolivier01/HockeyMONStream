#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::stitching {

enum class TransactionJournalKind {
  kRink,
  kStitch,
};

inline constexpr size_t kMaximumRinkTransactionArtifacts = 64;
inline constexpr uint64_t kMaximumRinkTransactionRollbackBytes = 1024ULL * 1024ULL * 1024ULL;

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

class PinnedRinkRollbackArtifact {
 public:
  PinnedRinkRollbackArtifact() = default;
  ~PinnedRinkRollbackArtifact();
  PinnedRinkRollbackArtifact(PinnedRinkRollbackArtifact&& other) noexcept;
  PinnedRinkRollbackArtifact& operator=(PinnedRinkRollbackArtifact&& other) noexcept;
  PinnedRinkRollbackArtifact(const PinnedRinkRollbackArtifact&) = delete;
  PinnedRinkRollbackArtifact& operator=(const PinnedRinkRollbackArtifact&) = delete;

  static absl::StatusOr<PinnedRinkRollbackArtifact> Open(const PinnedDirectory& directory, const std::string& name);

  const std::string& name() const {
    return name_;
  }
  uint64_t size() const {
    return size_;
  }

 private:
  PinnedRinkRollbackArtifact(int descriptor, std::string name, uint64_t size)
      : descriptor_(descriptor), name_(std::move(name)), size_(size) {}

  friend absl::Status snapshot_rink_artifact_for_rollback(
      const PinnedRinkRollbackArtifact& source,
      const std::filesystem::path& destination,
      bool force_portable_fallback);

  int descriptor_{-1};
  std::string name_;
  uint64_t size_{0};
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
    size_t maximum_bytes);

// Copies a previously pinned and bounded backup inode, preserving aggregate
// validation even if its directory entry is replaced during recovery.
absl::Status snapshot_rink_artifact_for_rollback(
    const PinnedRinkRollbackArtifact& source,
    const std::filesystem::path& destination,
    bool force_portable_fallback);

// Pins and validates one complete rollback set before any snapshot is staged.
absl::StatusOr<std::vector<PinnedRinkRollbackArtifact>> pin_rink_rollback_artifacts(
    const PinnedDirectory& directory,
    const std::vector<std::string>& names);

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
