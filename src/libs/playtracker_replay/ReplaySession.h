#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "hockeymon/csrc/play_tracker/BoxUtils.h"
#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerRuntimeConfig.h"

namespace hm::playtracker_replay {

struct Frame {
  uint64_t sample_id{0};
  uint64_t pts_ns{0};
  std::optional<hm::BBox> fast;
  std::optional<hm::BBox> follower;
  float edge_rotation_left{0};
  float edge_rotation_right{0};
};

struct PrepareOptions {
  std::string manifest_path;
  // Offset from the recording's first PTS, and exclusive duration. The first
  // sample at or after in is selected; out must not cross a reset/seek segment.
  double start_seconds{0};
  double duration_seconds{10};
  std::optional<hm::BBox> legacy_arena;
  // Optional explicit replacement for an absent archived effective config.
  // It is still required to pass measured trajectory agreement.
  std::string legacy_config_path;
  double legacy_tolerance_pixels{0.5};
};

struct MediaBinding {
  std::string path;
  uint64_t telemetry_origin_pts_ns{0};
  uint64_t video_origin_pts_ns{0};
  uint32_t width{0};
  uint32_t height{0};
};

struct TrialResult {
  std::string name;
  DsPlayTrackerRuntimeTuning tuning;
  std::vector<Frame> frames;
};

// This synchronous CPU API is intended to run on a worker thread. Prepared
// sessions are immutable; every trial restores its own independent tracker.
class ReplaySession {
 public:
  static absl::StatusOr<std::shared_ptr<ReplaySession>> Prepare(
      const PrepareOptions& options,
      const std::atomic<bool>* cancelled = nullptr);
  absl::StatusOr<TrialResult> RunTrial(
      std::string name,
      const DsPlayTrackerRuntimeTuning& tuning,
      const std::atomic<bool>* cancelled = nullptr) const;
  absl::Status SaveTrial(const std::string& path, const TrialResult& trial, const MediaBinding& media) const;

  const std::vector<Frame>& original() const;
  const std::vector<Frame>& baseline() const;
  uint32_t width() const;
  uint32_t height() const;
  uint64_t end_pts_ns() const;
  const std::string& provenance() const;
  const std::string& manifest_path() const;

 private:
  struct Impl;
  explicit ReplaySession(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
};

} // namespace hm::playtracker_replay
