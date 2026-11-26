#include "hstream/src/libs/scoreboard/CameraFrames.h"

#include <stdexcept>

namespace hm::scoreboard {
CameraFrames::CameraFrames(size_t capacity) : capacity_(capacity) {
  if (!capacity)
    throw std::invalid_argument("Scoreboard handoff requires positive capacity");
}
bool CameraFrames::Reset(uint64_t epoch) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (epoch <= epoch_)
    return false;
  epoch_ = epoch;
  published_end_ns_ = selected_ns_ = -1;
  frames_.clear();
  current_.reset();
  ended_ = cancelled_ = false;
  changed_.notify_all();
  return true;
}
CameraFrames::PublishResult CameraFrames::TryPublish(const Frame& frame) {
  if (!frame.image || frame.time_ns < 0 || frame.end_ns <= frame.time_ns)
    return PublishResult::kInvalid;
  std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock())
    return PublishResult::kFull;
  return PublishLocked(frame);
}
CameraFrames::PublishResult CameraFrames::PublishLocked(const Frame& frame) {
  if (!epoch_ || frame.epoch != epoch_)
    return PublishResult::kObsolete;
  if (cancelled_)
    return PublishResult::kCancelled;
  if (ended_ || frame.time_ns < published_end_ns_)
    return PublishResult::kInvalid;
  if (frames_.size() >= capacity_)
    return PublishResult::kFull;
  published_end_ns_ = frame.end_ns;
  frames_.push_back(std::make_shared<const Frame>(frame));
  changed_.notify_all();
  return PublishResult::kPublished;
}
CameraFrames::PublishResult CameraFrames::WaitPublish(const Frame& frame, std::chrono::nanoseconds timeout) {
  if (!frame.image || frame.time_ns < 0 || frame.end_ns <= frame.time_ns)
    return PublishResult::kInvalid;
  std::unique_lock<std::mutex> lock(mutex_);
  auto result = PublishLocked(frame);
  if (result != PublishResult::kFull)
    return result;
  if (timeout.count() <= 0)
    return PublishResult::kTimeout;
  const auto now = std::chrono::steady_clock::now();
  const auto remaining = std::chrono::steady_clock::time_point::max() - now;
  const auto deadline = timeout >= remaining ? std::chrono::steady_clock::time_point::max() : now + timeout;
  const bool available = changed_.wait_until(lock, deadline, [&] {
    result = PublishLocked(frame);
    return result != PublishResult::kFull;
  });
  return available ? result : PublishResult::kTimeout;
}
bool CameraFrames::End(uint64_t epoch) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!epoch_ || epoch != epoch_ || cancelled_)
    return false;
  ended_ = true;
  changed_.notify_all();
  return true;
}
void CameraFrames::Cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  cancelled_ = true;
  changed_.notify_all();
}
CameraFrames::Selection CameraFrames::SelectLocked(uint64_t epoch, int64_t game_time_ns) {
  if (!epoch_ || epoch != epoch_ || game_time_ns < selected_ns_ || game_time_ns < 0)
    return {SelectionStatus::kObsolete, {}};
  if (cancelled_)
    return {SelectionStatus::kCancelled, {}};
  selected_ns_ = game_time_ns;
  while (!frames_.empty() && frames_.front()->time_ns <= game_time_ns) {
    current_ = std::move(frames_.front());
    frames_.pop_front();
    changed_.notify_all();
  }
  if (current_ && game_time_ns < current_->end_ns)
    return {SelectionStatus::kReady, current_};
  if (!frames_.empty())
    return {SelectionStatus::kGap, {}};
  return {ended_ ? SelectionStatus::kEnded : SelectionStatus::kPending, {}};
}
CameraFrames::Selection CameraFrames::TrySelect(uint64_t epoch, int64_t game_time_ns) {
  std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock())
    return {SelectionStatus::kBusy, {}};
  return SelectLocked(epoch, game_time_ns);
}
CameraFrames::Selection CameraFrames::WaitSelect(
    uint64_t epoch,
    int64_t game_time_ns,
    std::chrono::nanoseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  Selection result = SelectLocked(epoch, game_time_ns);
  if (result.status != SelectionStatus::kPending)
    return result;
  if (timeout.count() <= 0)
    return {SelectionStatus::kTimeout, {}};
  const auto now = std::chrono::steady_clock::now();
  const auto remaining = std::chrono::steady_clock::time_point::max() - now;
  const auto deadline = timeout >= remaining ? std::chrono::steady_clock::time_point::max() : now + timeout;
  const bool available = changed_.wait_until(lock, deadline, [&] {
    result = SelectLocked(epoch, game_time_ns);
    return result.status != SelectionStatus::kPending;
  });
  return available ? result : Selection{SelectionStatus::kTimeout, {}};
}
} // namespace hm::scoreboard
