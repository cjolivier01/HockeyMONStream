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
  uint64_t team_enqueues{0};
  uint64_t team_results{0};
  // Embeddings the clusterer declined because the two centroids were too close
  // to call for this crop.
  uint64_t team_unassigned{0};
  // Embeddings that arrived before the split could be named at all. Counted
  // apart from the margin refusals above so a run that is still waiting for a
  // publishable split does not read like one whose crops are all ambiguous.
  uint64_t team_unranked{0};
  // Times a later fit overturned the dark/light ranking. Not an error: it is
  // how an early ranking taken on a marginal gap gets corrected.
  uint64_t team_reranks{0};
  // Tracks that newly committed to a side. The only counter that distinguishes
  // a run which labelled players from one that only ever ran the model.
  uint64_t team_committed{0};
  uint64_t team_size_skipped{0};
  uint64_t team_visibility_skipped{0};
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
