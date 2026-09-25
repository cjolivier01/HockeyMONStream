#pragma once

#include <nvbufsurface.h>
#include <nvdsmeta.h>

#include <cstdint>
#include <memory>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "hstream/src/libs/player_analytics/Config.h"

namespace hm::player_analytics {

struct AnalyticsCounters {
  uint64_t frames{0};
  uint64_t pose_enqueues{0};
  uint64_t pose_results{0};
  uint64_t jersey_enqueues{0};
  uint64_t jersey_results{0};
  uint64_t action_enqueues{0};
  uint64_t action_results{0};
  uint64_t model_samples{0};
  uint64_t maximum_frame_samples{0};
  uint64_t budget_deferred{0};
  uint64_t jersey_pose_skipped{0};
  uint64_t jersey_visibility_skipped{0};
  uint64_t action_history_unready{0};
  uint64_t action_history_resets{0};
  uint64_t invalid_time{0};
  uint64_t invalid_rois{0};
  uint64_t capacity_excluded{0};
  uint64_t duplicate_time{0};
  uint64_t cancelled_batches{0};
};

// Synchronous metadata processor. A borrowed video surface is never retained
// after Process returns, including failure. Only compact semantic outputs leave
// the GPU; frames and person crops remain device resident.
class PlayerAnalyticsProcessor {
 public:
  static absl::StatusOr<std::unique_ptr<PlayerAnalyticsProcessor>> Create(const Config& config, int gpu_id);
  ~PlayerAnalyticsProcessor();
  absl::Status Process(NvBufSurface* surface, NvDsBatchMeta* batch);
  // Thread-safe; FLUSH_START retires this publication epoch without waiting for
  // GPU work. Process fences old work and returns Cancelled. Reset must follow
  // on the serialized streaming path before accepting new frames.
  void CancelPending() noexcept;
  // Only a serialized FLUSH_STOP may clear a pending flush. Discontinuity or
  // STREAM_START resets must not reopen publication during overlapping flush.
  void Reset(bool finish_flush = false) noexcept;
  const AnalyticsCounters& counters() const noexcept;

 private:
  struct Impl;
  explicit PlayerAnalyticsProcessor(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace hm::player_analytics
