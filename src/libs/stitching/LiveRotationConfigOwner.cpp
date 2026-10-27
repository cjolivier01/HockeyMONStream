#include "hstream/src/libs/stitching/LiveRotationConfigOwner.h"

#include <utility>

namespace hm::stitching {
namespace {

bool SameAuthorization(
    const LiveRotationConfigOwner::Authorization& left,
    const LiveRotationConfigOwner::Authorization& right) {
  return left.game_dir == right.game_dir && left.value.pending_generation == right.value.pending_generation &&
      left.value.authorization_id == right.value.authorization_id;
}

} // namespace

LiveRotationConfigOwner::LiveRotationConfigOwner(std::function<void()> wake)
    : wake_(std::move(wake)), worker_([this]() { Run(); }) {}

LiveRotationConfigOwner::~LiveRotationConfigOwner() {
  (void)Shutdown();
}

bool LiveRotationConfigOwner::Enqueue(Work work, Complete complete) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_)
    return false;
  work_.push_back({std::move(work), std::move(complete)});
  ++pending_;
  changed_.notify_one();
  return true;
}

bool LiveRotationConfigOwner::Authorize(std::string game_dir, double rotation, std::string id, Complete complete) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || retiring_.count(game_dir))
    return false;
  work_.push_back(
      {[this, game_dir = std::move(game_dir), rotation, id = std::move(id)]() {
         auto result = authorize_live_stitched_output_rotation(game_dir, rotation, id);
         if (!result.ok())
           return Result{result.status(), std::nullopt};
         Authorization authorization{game_dir, *result};
         if (authorization.value)
           active_[game_dir] = authorization;
         else
           active_.erase(game_dir);
         return Result{absl::OkStatus(), std::move(authorization)};
       },
       std::move(complete)});
  ++pending_;
  changed_.notify_one();
  return true;
}

bool LiveRotationConfigOwner::Commit(Authorization authorization, Complete complete) {
  return Enqueue(
      [this, authorization = std::move(authorization)]() {
        const auto& value = authorization.value;
        const auto status = commit_live_stitched_output_rotation(
            authorization.game_dir, value.pending_generation, value.authorization_id);
        // An invalidating commit deliberately retains its pending slot until
        // the producer publishes the new generation. Retirement owns it too.
        if (status.ok() && !value.invalidate_scoreboard) {
          const auto active = active_.find(authorization.game_dir);
          if (active != active_.end() && SameAuthorization(active->second, authorization))
            active_.erase(active);
        }
        return Result{status, authorization};
      },
      std::move(complete));
}

LiveRotationConfigOwner::Result LiveRotationConfigOwner::RollbackOwned(
    const Authorization& authorization,
    bool unwind) {
  Authorization current = authorization;
  for (;;) {
    auto result = rollback_live_stitched_output_rotation(
        current.game_dir, current.value.pending_generation, current.value.authorization_id);
    if (!result.ok())
      return {result.status(), current};
    const auto active = active_.find(current.game_dir);
    const bool owns_active = active != active_.end() && SameAuthorization(active->second, current);
    if (result->has_value()) {
      current.value = **result;
      if (owns_active)
        active_[current.game_dir] = current;
      if (unwind)
        continue;
      return {absl::OkStatus(), std::move(current)};
    }
    if (owns_active)
      active_.erase(active);
    return {absl::OkStatus(), std::nullopt};
  }
}

bool LiveRotationConfigOwner::Rollback(Authorization authorization, Complete complete) {
  return Enqueue(
      [this, authorization = std::move(authorization)]() { return RollbackOwned(authorization, false); },
      std::move(complete));
}

bool LiveRotationConfigOwner::Retire(std::string game_dir, Complete complete) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || retirement_pending_.count(game_dir))
    return false;
  retiring_.insert(game_dir);
  retirement_pending_.insert(game_dir);
  retired_.erase(game_dir);
  work_.push_back(
      {[this, game_dir]() {
         const auto active = active_.find(game_dir);
         return active == active_.end() ? Result{absl::OkStatus(), std::nullopt} : RollbackOwned(active->second, true);
       },
       [this, game_dir, complete = std::move(complete)](Result result) mutable {
         {
           std::lock_guard<std::mutex> lock(mutex_);
           retirement_pending_.erase(game_dir);
           if (result.status.ok())
             retired_.insert(game_dir);
         }
         if (complete)
           complete(std::move(result));
       }});
  ++pending_;
  changed_.notify_one();
  return true;
}

bool LiveRotationConfigOwner::AllowGame(const std::string& game_dir) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || retirement_pending_.count(game_dir) || (retiring_.count(game_dir) && !retired_.count(game_dir)))
    return false;
  retiring_.erase(game_dir);
  retired_.erase(game_dir);
  return true;
}

void LiveRotationConfigOwner::Poll() {
  std::deque<std::pair<Complete, Result>> completed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    completed.swap(completed_);
    pending_ -= completed.size();
  }
  for (auto& [complete, result] : completed)
    if (complete)
      complete(std::move(result));
}

bool LiveRotationConfigOwner::pending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_ != 0;
}

absl::Status LiveRotationConfigOwner::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    changed_.notify_one();
  }
  if (worker_.joinable())
    worker_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  completed_.clear();
  retirement_pending_.clear();
  pending_ = 0;
  return shutdown_status_;
}

void LiveRotationConfigOwner::Run() {
  for (;;) {
    Task task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [this]() { return stopping_ || !work_.empty(); });
      if (work_.empty())
        break;
      task = std::move(work_.front());
      work_.pop_front();
    }
    Result result = task.work();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      completed_.emplace_back(std::move(task.complete), std::move(result));
    }
    if (wake_)
      wake_();
  }
  while (!active_.empty()) {
    const Authorization authorization = active_.begin()->second;
    const Result result = RollbackOwned(authorization, true);
    if (!result.status.ok()) {
      shutdown_status_.Update(result.status);
      // Preserve the first failure and continue retiring other games. Startup
      // reconciliation can retry this exact on-disk process-owned slot.
      active_.erase(authorization.game_dir);
    }
  }
}

} // namespace hm::stitching
