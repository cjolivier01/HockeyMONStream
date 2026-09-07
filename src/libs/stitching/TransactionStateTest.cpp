#include "hstream/src/libs/stitching/TransactionState.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

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

  const fs::path invalid = root / ".hstream-rink-recovery-pending";
  fs::remove(invalid);
  fs::create_directory(invalid);
  auto malformed =
      hm::stitching::transaction_recovery_scan_required(root, hm::stitching::TransactionJournalKind::kRink);
  ok &= expect(!malformed.ok(), "non-regular recovery markers must fail closed");

  fs::remove_all(root);
  return ok ? 0 : 1;
}
