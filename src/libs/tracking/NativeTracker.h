#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "absl/status/statusor.h"
#include "hstream/src/libs/detection/DetectionTypes.h"

namespace hm::tracking {

// Explicit metadata-only tracker tuning. The application must resolve every
// value from the shared baseline and selected overrides before constructing it.
struct TrackerConfig {
  float low_score, high_score, new_track_score;
  float high_match_cost, low_match_cost, tentative_match_cost;
  bool weight_high_matches_by_score;
  uint64_t keep_lost_frames;
  // Matrix bytes are bounded separately from the O(maximum_tracks) state and
  // assignment scratch. Both track and per-frame observation counts are capped.
  size_t confirmation_frames, maximum_tracks, maximum_cost_matrix_bytes;
};
absl::Status ValidateTrackerConfig(const TrackerConfig& config);
struct Observation {
  uint64_t id;
  detection::Proposal box;
};

// Geometric two-stage association with the shared native Kalman filter.
// High-score detections can recover lost tracks; low-score detections only
// maintain tracks observed in the previous frame. Lost predictions never
// masquerade as current observations. No Torch, GPU or video dependency.
class NativeTracker {
 public:
  static absl::StatusOr<std::unique_ptr<NativeTracker>> Create(const TrackerConfig& config);
  ~NativeTracker();
  NativeTracker(const NativeTracker&) = delete;
  NativeTracker& operator=(const NativeTracker&) = delete;

  // Sequential frame indices are mandatory, including frames with no objects.
  // Call Reset after seeks/source discontinuities. Reset preserves the ID
  // counter, so a new track cannot reuse an old observation's identity.
  absl::StatusOr<std::vector<Observation>> Update(
      uint64_t sequence,
      const std::vector<detection::Proposal>& detections);
  void Reset();

 private:
  struct Impl;
  explicit NativeTracker(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
} // namespace hm::tracking
