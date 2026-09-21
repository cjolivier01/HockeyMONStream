#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::pipeline {

struct PlayerFrameScanProgress {
  std::optional<uint64_t> elapsed_ns;
  bool boundary_observed{false};
};

// The decoder may seek or run ahead while inference is starting. Only successfully
// observed frames advance scan progress; passing the gate never completes a scan.
class PlayerFrameScanTiming {
 public:
  PlayerFrameScanTiming(uint64_t duration_ns, uint64_t interval_ns)
      : duration_ns_(duration_ns), interval_ns_(interval_ns) {}

  absl::StatusOr<bool> Select(uint64_t pts) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!first_pts_)
      first_pts_ = pts;
    if (pts < *first_pts_ || (last_sample_pts_ && pts < *last_sample_pts_))
      return absl::FailedPreconditionError("Player frame scan: source timeline went backwards");
    if (pts - *first_pts_ >= duration_ns_) {
      if (boundary_sent_)
        return false;
      boundary_sent_ = true;
      return true;
    }
    if (last_sample_pts_ && pts - *last_sample_pts_ < interval_ns_)
      return false;
    last_sample_pts_ = pts;
    return true;
  }

  absl::StatusOr<uint64_t> Elapsed(uint64_t pts) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return elapsed_locked(pts);
  }

  // Call after inference/mask validation and scoring, or after validating the
  // unscored boundary frame. Queued samples ahead of that frame have then drained.
  absl::Status Observe(uint64_t pts) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto elapsed = elapsed_locked(pts);
    if (!elapsed.ok())
      return elapsed.status();
    progress_.elapsed_ns = *elapsed;
    progress_.boundary_observed = *elapsed >= duration_ns_;
    return absl::OkStatus();
  }

  PlayerFrameScanProgress progress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
  }

 private:
  absl::StatusOr<uint64_t> elapsed_locked(uint64_t pts) const {
    if (!first_pts_ || pts < *first_pts_)
      return absl::FailedPreconditionError("Player frame scan: observed frame has no matching scan timeline");
    const uint64_t elapsed = pts - *first_pts_;
    if (progress_.elapsed_ns && elapsed < *progress_.elapsed_ns)
      return absl::FailedPreconditionError("Player frame scan: inference observations went backwards");
    return elapsed;
  }

  const uint64_t duration_ns_;
  const uint64_t interval_ns_;
  mutable std::mutex mutex_;
  std::optional<uint64_t> first_pts_;
  std::optional<uint64_t> last_sample_pts_;
  bool boundary_sent_{false};
  PlayerFrameScanProgress progress_;
};

inline bool PlayerFrameScanCompletedCleanly(
    const PlayerFrameScanProgress& progress,
    bool interrupted,
    bool natural_eos) {
  return !interrupted && (progress.boundary_observed || natural_eos);
}

} // namespace hm::pipeline
