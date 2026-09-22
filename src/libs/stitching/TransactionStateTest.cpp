#include "hstream/src/libs/stitching/TransactionState.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

bool exercise_marker_protocol(const fs::path& root, hm::stitching::TransactionJournalKind kind) {
  auto legacy = hm::stitching::transaction_recovery_scan_required(root, kind);
  auto completed = hm::stitching::complete_transaction_recovery(root, kind);
  auto steady = hm::stitching::transaction_recovery_scan_required(root, kind);
  const fs::path legacy_transaction = root /
      (kind == hm::stitching::TransactionJournalKind::kRink ? ".hstream-rink-legacy-after-protocol"
                                                            : ".hstream-stitch-legacy-after-protocol");
  fs::create_directory(legacy_transaction);
  auto legacy_after_protocol = hm::stitching::transaction_recovery_scan_required(root, kind);
  fs::remove(legacy_transaction);
  auto legacy_completed = hm::stitching::complete_transaction_recovery(root, kind);
  auto pending = hm::stitching::mark_transaction_recovery_pending(root, kind);
  auto active = hm::stitching::transaction_recovery_scan_required(root, kind);
  auto recovered = hm::stitching::complete_transaction_recovery(root, kind);
  auto final = hm::stitching::transaction_recovery_scan_required(root, kind);
  return legacy.ok() && *legacy && completed.ok() && steady.ok() && !*steady && legacy_after_protocol.ok() &&
      *legacy_after_protocol && legacy_completed.ok() && pending.ok() && active.ok() && *active && recovered.ok() &&
      final.ok() && !*final;
}

bool exercise_directory_removal_retry(const fs::path& root, const char* failure) {
  const fs::path directory = root / (std::string("removal-retry-") + failure);
  fs::create_directory(directory);
  const auto owned = hm::stitching::write_owned_directory_marker(directory, "journal_version", "2\n");
  std::ofstream(directory / "payload") << "transaction payload\n";
  const bool disappearing_entry = std::string(failure) == "disappearing";
  bool removed_late_entry = !disappearing_entry;
  std::thread late_entry_cleanup;
  if (disappearing_entry) {
    late_entry_cleanup = std::thread([&]() {
      const fs::path entry = directory / ".injected-concurrent-entry";
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      std::error_code error;
      while (std::chrono::steady_clock::now() < deadline) {
        if (fs::exists(entry, error)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(30));
          removed_late_entry = fs::remove(entry, error) && !error;
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }
  ::setenv("HM_TEST_TRANSACTION_RMDIR_FAILURE", disappearing_entry ? "nonempty" : failure, 1);
  const auto removed = hm::stitching::remove_owned_directory(directory, "journal_version", "2\n");
  ::unsetenv("HM_TEST_TRANSACTION_RMDIR_FAILURE");
  if (late_entry_cleanup.joinable())
    late_entry_cleanup.join();
  bool ok = expect(owned.ok(), "retry fixture must have a durable ownership marker");
  if (std::string(failure) == "transient" || disappearing_entry) {
    ok &= expect(
        removed.ok() && removed_late_entry && !fs::exists(directory),
        "transient ENOTEMPTY or a disappearing late entry must finish cleanup without deleting new entries");
    if (!ok)
      std::cerr << failure << " cleanup status: " << removed << '\n';
    return ok;
  }
  const auto retained = hm::stitching::owned_directory_marker_matches(directory, "journal_version", "2\n");
  ok &= expect(
      !removed.ok() && retained.ok() && *retained && !fs::exists(directory / "payload"),
      "failed final directory removal must restore ownership after deleting transaction payloads");
  if (std::string(failure) == "nonempty") {
    ok &= expect(
        absl::IsAborted(removed) && fs::is_regular_file(directory / ".injected-concurrent-entry"),
        "cleanup must fail without deleting an entry that appeared after payload removal");
  } else {
    ok &= expect(
        absl::IsInternal(removed),
        "persistent ENOTEMPTY must stop after bounded retries rather than reporting successful cleanup");
  }
  const auto resumed = hm::stitching::remove_owned_directory(directory, "journal_version", "2\n");
  ok &= expect(resumed.ok() && !fs::exists(directory), "restored ownership must allow a later cleanup to finish");
  if (!ok)
    std::cerr << failure << " cleanup status: " << removed << "; resumed status: " << resumed << '\n';
  return ok;
}

bool exercise_busy_nfs_entry(const fs::path& root, bool nested) {
  const fs::path directory = root / (nested ? "busy-nfs-nested" : "busy-nfs-root");
  const fs::path contents = nested ? directory / "previous" : directory;
  fs::create_directories(contents);
  const auto owned = hm::stitching::write_owned_directory_marker(directory, "journal_version", "2\n");
  const std::string tombstone_name = ".nfs000000000000000100000001";
  const fs::path tombstone = contents / tombstone_name;
  std::ofstream(tombstone) << "previously unlinked marker\n";
  bool removed_tombstone = false;
  std::thread release_tombstone;
  if (!nested) {
    release_tombstone = std::thread([&]() {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      std::error_code error;
      while (std::chrono::steady_clock::now() < deadline) {
        if (!fs::exists(directory / "journal_version", error) && !error) {
          std::this_thread::sleep_for(std::chrono::milliseconds(30));
          removed_tombstone = fs::remove(tombstone, error) && !error;
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }
  ::setenv("HM_TEST_TRANSACTION_BUSY_NFS_ENTRY", tombstone_name.c_str(), 1);
  const auto removed = hm::stitching::remove_owned_directory(directory, "journal_version", "2\n");
  ::unsetenv("HM_TEST_TRANSACTION_BUSY_NFS_ENTRY");
  if (release_tombstone.joinable())
    release_tombstone.join();
  bool ok = expect(owned.ok(), "busy NFS fixture must have a durable ownership marker");
  if (nested) {
    const auto retained = hm::stitching::owned_directory_marker_matches(directory, "journal_version", "2\n");
    ok &= expect(
        absl::IsAborted(removed) && fs::is_regular_file(tombstone) && retained.ok() && *retained,
        "a busy nested NFS tombstone must exhaust the emptiness wait while preserving the entry and ownership");
    fs::remove(tombstone);
    const auto resumed = hm::stitching::remove_owned_directory(directory, "journal_version", "2\n");
    ok &= expect(
        resumed.ok() && !fs::exists(directory), "nested cleanup must resume after the NFS reader releases its file");
    if (!resumed.ok())
      std::cerr << "Nested NFS resumed cleanup status: " << resumed << '\n';
  } else {
    ok &= expect(
        removed.ok() && removed_tombstone && !fs::exists(directory),
        "a pre-existing busy NFS tombstone must be left for its reader to release before removing its parent");
  }
  if (!ok)
    std::cerr << "Busy NFS cleanup status (nested=" << nested << "): " << removed << '\n';
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  const fs::path root = fs::temp_directory_path() / ("transaction-state-test-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);

  ok &= expect(
      exercise_marker_protocol(root, hm::stitching::TransactionJournalKind::kRink),
      "rink recovery markers must scan legacy/pending roots and skip steady-state roots");
  ok &= expect(
      exercise_marker_protocol(root, hm::stitching::TransactionJournalKind::kStitch),
      "stitch recovery markers must scan legacy/pending roots and skip steady-state roots");

  ::setenv("HM_TEST_FORCE_TRANSACTION_RECOVERY_SCAN", "1", 1);
  auto forced = hm::stitching::transaction_recovery_scan_required(root, hm::stitching::TransactionJournalKind::kRink);
  ::unsetenv("HM_TEST_FORCE_TRANSACTION_RECOVERY_SCAN");
  ok &= expect(forced.ok() && *forced, "test override must force compatibility recovery scanning");

  const fs::path owned = root / "hstream-stitch-ABC123";
  fs::create_directory(owned);
  const auto awaiting = hm::stitching::publish_transaction_state(owned, "AWAITING_CONFIG\n");
  std::ifstream state_file(owned / "state");
  std::string state;
  std::getline(state_file, state);
  state_file.close();
  ok &= expect(awaiting.ok() && state == "AWAITING_CONFIG", "selection publication must durably enter AWAITING_CONFIG");
  ok &= expect(!hm::stitching::publish_transaction_state(owned, "UNKNOWN\n").ok(), "unknown journal states must fail");
  std::ofstream(owned / "first") << "first\n";
  std::ofstream(owned / "second") << "second\n";
  auto owner_status = hm::stitching::write_owned_directory_marker(owned, "journal_version", "2\n");
  auto pinned_root = hm::stitching::PinnedDirectory::Open(root, "transaction cleanup test root");
  auto pinned_owned = pinned_root.ok()
      ? pinned_root->OpenChild(owned.filename().string(), "owned transaction cleanup test directory")
      : absl::StatusOr<std::optional<hm::stitching::PinnedDirectory>>(pinned_root.status());
  ::setenv("HM_TEST_TRANSACTION_CLEANUP_INTERRUPT_AFTER_ENTRY", "1", 1);
  const auto interrupted_cleanup = pinned_owned.ok() && pinned_owned->has_value()
      ? hm::stitching::remove_pinned_directory(
            *pinned_root, owned.filename().string(), **pinned_owned, "journal_version")
      : pinned_owned.status();
  ::unsetenv("HM_TEST_TRANSACTION_CLEANUP_INTERRUPT_AFTER_ENTRY");
  ok &= expect(
      owner_status.ok() && !interrupted_cleanup.ok() && fs::is_regular_file(owned / "journal_version"),
      "interrupted cleanup must retain visible work-directory ownership until payload deletion completes");
  const auto resumed_cleanup = pinned_owned.ok() && pinned_owned->has_value()
      ? hm::stitching::remove_pinned_directory(
            *pinned_root, owned.filename().string(), **pinned_owned, "journal_version")
      : pinned_owned.status();
  ok &= expect(resumed_cleanup.ok() && !fs::exists(owned), "owned visible work-directory cleanup must be resumable");

  ok &= exercise_directory_removal_retry(root, "transient");
  ok &= exercise_directory_removal_retry(root, "disappearing");
  ok &= exercise_directory_removal_retry(root, "nonempty");
  ok &= exercise_directory_removal_retry(root, "exhausted");
  ok &= exercise_busy_nfs_entry(root, false);
  ok &= exercise_busy_nfs_entry(root, true);

  const fs::path invalid = root / ".hstream-rink-recovery-pending";
  fs::remove(invalid);
  fs::create_directory(invalid);
  auto malformed =
      hm::stitching::transaction_recovery_scan_required(root, hm::stitching::TransactionJournalKind::kRink);
  ok &= expect(!malformed.ok(), "non-regular recovery markers must fail closed");

  if (ok) {
    std::error_code error;
    fs::remove_all(root, error);
    if (error) {
      std::cerr << "Unable to remove test fixture " << root << ": " << error.message() << '\n';
      ok = false;
    }
  } else {
    std::cerr << "Keeping failed test fixture for inspection: " << root << '\n';
  }
  return ok ? 0 : 1;
}
