#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

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

// Local UI/CLI owners use a unique inherited capability to find helpers even
// after they create a new POSIX session. Remote workflow retirement never uses
// this local-only proof. Registration retains the owner's original ancestor
// identities. An unreadable process is excluded only with pinned ancestry proof
// through an unrelated process predating admission; orphans and uncertain
// metadata stay reserved. A process without that registration has no proof.
absl::StatusOr<bool> LocalChildrenStopped(const std::string& token);
absl::Status SignalLocalChildren(const std::string& token, int signal);
absl::Status ReleaseLocal(const std::filesystem::path& game, const std::string& token, bool cleanup_complete);

} // namespace hm::game_operation
