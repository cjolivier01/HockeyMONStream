#pragma once

#include "hstream/src/libs/stitching/LiveStitchingGeneration.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace hm::stitching {

// One serial disk owner is shared by Qt and headless runtime adapters. Poll
// delivers completions on the caller's event loop; the worker performs no
// pipeline or UI work. The optional wake callback only posts a notification to
// that loop. Shutdown joins work and unwinds the exact pending authorizations.
class LiveRotationConfigOwner {
 public:
  struct Authorization {
    std::string game_dir;
    LiveStitchedOutputAuthorization value;
  };
  struct Result {
    absl::Status status;
    std::optional<Authorization> authorization;
  };
  using Complete = std::function<void(Result)>;

  explicit LiveRotationConfigOwner(std::function<void()> wake = {});
  ~LiveRotationConfigOwner();
  LiveRotationConfigOwner(const LiveRotationConfigOwner&) = delete;
  LiveRotationConfigOwner& operator=(const LiveRotationConfigOwner&) = delete;

  bool Authorize(std::string game_dir, double rotation, std::string id, Complete complete);
  bool Commit(Authorization authorization, Complete complete);
  bool Rollback(Authorization authorization, Complete complete);
  // Stop forbids new authorizations for this game until AllowGame is called.
  // Already queued/in-flight work is reconciled before the retirement reply.
  bool Retire(std::string game_dir, Complete complete);
  bool AllowGame(const std::string& game_dir);

  void Poll();
  bool pending() const;
  // Normal callers retire asynchronously and wait for pending()==false before
  // destruction. This final barrier also handles forced owner destruction.
  absl::Status Shutdown();

 private:
  using Work = std::function<Result()>;
  struct Task {
    Work work;
    Complete complete;
  };
  bool Enqueue(Work work, Complete complete);
  Result RollbackOwned(const Authorization& authorization, bool unwind);
  void Run();

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Task> work_;
  std::deque<std::pair<Complete, Result>> completed_;
  std::set<std::string> retiring_;
  std::set<std::string> retirement_pending_;
  std::set<std::string> retired_;
  std::map<std::string, Authorization> active_;
  size_t pending_{0};
  bool stopping_{false};
  absl::Status shutdown_status_;
  // Only the serial worker accesses active_. The main-thread admission gates
  // and completion queues are protected by mutex_.
  std::function<void()> wake_;
  std::thread worker_;
};

} // namespace hm::stitching
