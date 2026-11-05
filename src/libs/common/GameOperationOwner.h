#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::game_operation {

// This is a durable admission claim, not a process-liveness assertion. In
// particular, a different host cannot retire it by inspecting its local /proc.
struct Owner {
  std::string kind;
  std::string id;
  std::string token;
  std::string host;
  std::string boot_id;
  std::string workflow_directory;
  int64_t process_id{0};
  uint64_t revision{0};
  bool cancel_requested{false};
};

// Acquire before reading/writing mutable game state. Keep this lock until that
// transaction commits. Durable operations release the lock after Claim and keep
// the owner record until their supervisor proves all work and cleanup stopped.
class Lock final {
 public:
  ~Lock();
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
  static absl::StatusOr<std::unique_ptr<Lock>> Acquire(const std::filesystem::path& game, bool wait = true);
  absl::StatusOr<std::optional<Owner>> Read() const;
  absl::Status Authorize(const std::string& token) const;
  absl::Status Claim(Owner& owner);
  absl::Status Handoff(Owner& successor, const std::string& expected_token, uint64_t expected_revision);
  absl::Status RequestCancel(const std::string& token, uint64_t expected_revision);
  // The caller is the owning supervisor and must already have exact shutdown
  // and cleanup proof. A false proof and stale token/revision always fail.
  absl::Status Release(const std::string& token, uint64_t expected_revision, bool stopped_and_clean);

 private:
  Lock(int directory, int descriptor) : directory_(directory), descriptor_(descriptor) {}
  absl::Status Write(const Owner& owner) const;
  int directory_{-1};
  int descriptor_{-1};
};

absl::StatusOr<std::optional<Owner>> Inspect(const std::filesystem::path& game);
absl::StatusOr<std::unique_ptr<Lock>> Mutation(const std::filesystem::path& game, bool wait = true);
absl::StatusOr<std::string> NewToken();
Owner LocalOwner(const std::string& kind, const std::string& id, const std::string& token);

// Process-local authorization is registered only after admission. Children
// receive the exact game/token through HSTREAM_GAME_OPERATION_GAME/TOKEN.
void RegisterAuthorization(const std::filesystem::path& game, const std::string& token);
void ForgetAuthorization(const std::filesystem::path& game, const std::string& token);
std::string Authorization(const std::filesystem::path& game);

// Dedicated CLI processes call this before launching any child. At retirement,
// the caller must have stopped its own launchers and child-status consumers.
// This enables subreaping and permits reaping every child until kernel ECHILD.
// Never use it in a shared Qt process: use per-command guards below instead.
absl::Status EnableIsolatedChildProof();
absl::Status RetireIsolatedChildren(int64_t grace_ms, int64_t poll_ms);

// The UI fences new launches and routes every operation-owned command through
// a guard. A registered authorization alone never proves descendant shutdown.
absl::Status EnableSupervisedChildProof(const std::string& token);
struct SupervisedCommand {
  std::string id;
  std::vector<std::string> arguments;
  // A command may operate on a private candidate instead of the recording
  // that owns the outer operation. Its admission retires with this receipt.
  std::filesystem::path command_game;
  std::string command_token;
};
absl::StatusOr<SupervisedCommand> PrepareSupervisedCommand(
    const std::filesystem::path& game,
    const std::string& token,
    int64_t grace_ms,
    int64_t poll_ms,
    const std::vector<std::string>& command,
    const std::filesystem::path& command_game = {});
absl::Status SupervisedCommandStarted(const std::string& token, const std::string& id);
absl::Status SupervisedCommandFinished(const std::string& token, const std::string& id);
absl::StatusOr<bool> SupervisedCommandStopped(const std::string& token, const std::string& id);
absl::StatusOr<bool> SupervisedCommandPaused(const std::string& token, const std::string& id);
// Only QProcess::FailedToStart (or an explicitly abandoned, never-started
// command) may cancel a permit. A launched/crashed guard requires its receipt.
absl::Status CancelUnlaunchedSupervisedCommand(const std::string& token, const std::string& id);
// SIGKILL requests kill descendants, while preserving the guard and its proof.
absl::Status SignalSupervisedCommand(const std::string& token, const std::string& id, int signal);

// Proof is kernel ECHILD in a dedicated scope, or the exact guard receipts.
// Process snapshots are never shutdown authority. Missing proof stays reserved.
absl::StatusOr<bool> LocalChildrenStopped(const std::string& token);
absl::Status SignalLocalChildren(const std::string& token, int signal);
absl::Status ReleaseLocal(const std::filesystem::path& game, const std::string& token, bool cleanup_complete);

} // namespace hm::game_operation
