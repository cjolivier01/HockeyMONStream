#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "hstream/src/libs/player_analytics/Types.h"

namespace hm::player_analytics {

enum class Feature : size_t { kPose = 0, kJersey = 1, kAction = 2 };

struct TrackRecord {
  uint64_t id{kUntrackedId};
  uint64_t incarnation{0};
  uint64_t last_seen{kInvalidTime};
  std::array<uint64_t, 3> last_attempt{{kInvalidTime, kInvalidTime, kInvalidTime}};
  bool visible{false};
};

// One source/epoch owner. Call only on enabled inference paths. Retention and
// cadence use stitched frame.buf_pts; chapter-local source_pts are unsuitable.
class TrackScheduler {
 public:
  explicit TrackScheduler(size_t maximum_tracks = kMaximumTracks, uint64_t retention_ns = 2 * kSecond);
  bool BeginFrame(uint64_t epoch, uint64_t pts_ns, const uint64_t* visible_ids, size_t count) noexcept;
  const TrackRecord* Find(uint64_t id) const noexcept;
  size_t SelectDue(
      Feature feature,
      uint64_t interval_ns,
      std::array<uint64_t, kMaximumDueRois>* output,
      size_t limit = kMaximumDueRois) const noexcept;
  bool MarkAttempt(Feature feature, uint64_t id) noexcept;
  void Reset() noexcept;
  uint64_t excluded_count() const noexcept {
    return excluded_count_;
  }

 private:
  std::array<TrackRecord, kMaximumTracks> tracks_{};
  size_t maximum_tracks_;
  uint64_t retention_ns_;
  uint64_t epoch_{0};
  uint64_t now_{kInvalidTime};
  uint64_t next_incarnation_{1};
  uint64_t excluded_count_{0};
};

class JerseyConsensus {
 public:
  // At most one observation per source timestamp. Unknown/invalid readings age
  // evidence without reinforcing it. Strings "0", "00", and "07" are distinct.
  bool Observe(uint64_t pts_ns, std::string_view text, float confidence, float minimum_confidence = 0.8F) noexcept;
  JerseyResult Result(uint64_t now_ns) noexcept;
  void Reset() noexcept;

 private:
  void Advance(uint64_t now_ns) noexcept;
  void Select() noexcept;
  std::array<double, 110> evidence_{};
  std::array<uint64_t, 110> observed_{};
  uint64_t now_{kInvalidTime};
  uint64_t last_attempt_{kInvalidTime};
  int current_{-1};
};

// Stores only normalized compact skeletons. Allocate per admitted action track,
// never for pose-only/jersey-only runs. Dense model tensors are built on demand.
class ActionHistory {
 public:
  static constexpr size_t kTensorFloats = 2 * kActionSamples * kCocoJoints * 3;
  bool Observe(uint64_t pts_ns, const Pose& pose, float metadata_width, float metadata_height) noexcept;
  bool ready(uint64_t now_ns) const noexcept;
  bool WriteTensor(uint64_t now_ns, float* output, size_t count) const noexcept;
  uint64_t window_start() const noexcept;
  uint64_t window_end() const noexcept;
  size_t size() const noexcept {
    return count_;
  }
  void Reset() noexcept;

 private:
  void Append(uint64_t pts_ns, const Pose& pose) noexcept;
  std::array<Pose, kActionSamples> samples_{};
  std::array<uint64_t, kActionSamples> times_{};
  Pose previous_{};
  uint64_t previous_pts_{kInvalidTime};
  uint64_t next_sample_{kInvalidTime};
  float width_{0};
  float height_{0};
  size_t count_{0};
  size_t next_{0};
};

class ActionLabelState {
 public:
  bool Observe(
      uint64_t pts_ns,
      uint64_t window_start_ns,
      int label,
      float confidence,
      float minimum_confidence = 0.5F) noexcept;
  ActionResult Result(uint64_t now_ns) noexcept;
  void Reset() noexcept;

 private:
  ActionResult current_;
  uint64_t last_attempt_{kInvalidTime};
  int contender_{-1};
  size_t contender_count_{0};
};

} // namespace hm::player_analytics
