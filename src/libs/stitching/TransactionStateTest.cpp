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

  const fs::path invalid = root / ".hstream-rink-recovery-pending";
  fs::remove(invalid);
  fs::create_directory(invalid);
  auto malformed =
      hm::stitching::transaction_recovery_scan_required(root, hm::stitching::TransactionJournalKind::kRink);
  ok &= expect(!malformed.ok(), "non-regular recovery markers must fail closed");

  fs::remove_all(root);
  return ok ? 0 : 1;
}
