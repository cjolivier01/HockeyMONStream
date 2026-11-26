#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

namespace hm::scoreboard {
// Bounded, exact-time handoff from the independent scoreboard decoder to the
// final compositor. Decode can run ahead; a missing matching frame makes the
// compositor wait, never substitute a stale/future image or omit the overlay.
class CameraFrames {
 public:
  struct Frame {
    uint64_t epoch;
    // Half-open presentation interval on the common game clock. The producer
    // establishes the end from source timing, not a wall-clock freshness guess.
    int64_t time_ns, end_ns;
    // Immutable GPU pixels, published only after producer GPU completion.
    std::shared_ptr<const void> image;
  };
  enum class PublishResult { kPublished, kFull, kObsolete, kInvalid, kCancelled, kTimeout };
  enum class SelectionStatus { kReady, kPending, kBusy, kObsolete, kGap, kEnded, kCancelled, kTimeout };
  struct Selection {
    SelectionStatus status;
    std::shared_ptr<const Frame> frame;
  };
  // Capacity counts queued frames in addition to the current interval. The
  // application supplies the resolved baseline capacity; no unbounded queue.
  explicit CameraFrames(size_t capacity);
  bool Reset(uint64_t epoch);
  PublishResult TryPublish(const Frame& frame);
  PublishResult WaitPublish(const Frame& frame, std::chrono::nanoseconds timeout);
  bool End(uint64_t epoch);
  // Permanent for this epoch. Reset starts a newer, independently fenced run.
  void Cancel();
  Selection TrySelect(uint64_t epoch, int64_t game_time_ns);
  // Final-compositor join. Timeout/cancellation is an explicit failure, not
  // permission to publish a game frame without its required scoreboard.
  Selection WaitSelect(uint64_t epoch, int64_t game_time_ns, std::chrono::nanoseconds timeout);

 private:
  PublishResult PublishLocked(const Frame& frame);
  Selection SelectLocked(uint64_t epoch, int64_t game_time_ns);
  const size_t capacity_;
  std::mutex mutex_;
  std::condition_variable changed_;
  uint64_t epoch_{0};
  int64_t published_end_ns_{-1}, selected_ns_{-1};
  bool ended_{false}, cancelled_{false};
  std::deque<std::shared_ptr<const Frame>> frames_;
  std::shared_ptr<const Frame> current_;
};
} // namespace hm::scoreboard
