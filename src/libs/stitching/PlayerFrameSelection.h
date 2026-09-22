#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <yaml-cpp/yaml.h>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::stitching {

inline constexpr uint64_t kPlayerFrameSecond = 1000000000ULL;
inline constexpr size_t kPlayerFrameMaximumPairs = 16;
inline constexpr size_t kPlayerFrameCoverageCells = 16 * 9;

struct PlayerFrameIdentity {
  // Canonical absolute physical file path; file:// URI decoding belongs at the GStreamer boundary.
  std::string path;
  uint64_t source_pts_ns{0};
  uint32_t source_id{0};
  uint64_t sequence{0};
};

struct PlayerFramePairIdentity {
  std::array<PlayerFrameIdentity, 2> cameras;
  uint64_t timeline_pts_ns{0};
};

struct PlayerFrameSourceBinding {
  std::string path;
  uint64_t size{0};
  int64_t modification_time_ns{0};
};

struct PlayerFrameSelectionSettings {
  size_t frame_count{4};
  uint64_t duration_ns{60 * kPlayerFrameSecond};
  uint64_t interval_ns{kPlayerFrameSecond / 2};
  uint64_t minimum_separation_ns{kPlayerFrameSecond};
  size_t maximum_observations{1200};
};

struct PlayerFrameBox {
  double left{0};
  double top{0};
  double width{0};
  double height{0};
  double confidence{0};
  int class_id{0};
};

struct PlayerFrameObservation {
  PlayerFramePairIdentity pair;
  // Sorted unique size-band * 144 + spatial-cell indices. Far/middle/near are apparent size bands.
  std::vector<uint16_t> coverage;
  std::array<size_t, 3> size_band_counts{};
  size_t eligible_people{0};
  double quality{0};
};

// Required nonempty keys: source_context, baseline_generation, output_generation,
// detector_identity, rink_mask_sha256, rink_mask_revision, fieldmask_settings,
// output_rotation_degrees. Values are stable identities or normalized settings;
// source_context binds camera/lens/offset settings and decode anchor, excluding the plan itself.
// Additional string keys (e.g. segmentation_identity) are retained and fingerprinted.
using PlayerFrameSelectionContext = std::map<std::string, std::string>;

struct PlayerFrameSelectionPlan {
  PlayerFrameSelectionSettings settings;
  std::vector<PlayerFrameSourceBinding> sources;
  PlayerFrameSelectionContext context;
  std::vector<PlayerFrameObservation> selected;
  std::string fingerprint;
};

struct PlayerFrameSelectionReport {
  size_t observation_count{0};
  std::array<size_t, 3> observed_size_band_counts{};
  std::optional<PlayerFrameSelectionPlan> plan;
  // Successful scan with insufficient evidence has no plan and a nonempty reason.
  std::string unavailable_reason;
};

absl::Status ValidatePlayerFrameSelectionSettings(const PlayerFrameSelectionSettings& settings);
absl::StatusOr<PlayerFrameSourceBinding> BindPlayerFrameSource(const std::filesystem::path& path);
absl::Status CanonicalizePlayerFramePair(PlayerFramePairIdentity* pair);
absl::Status ValidatePlayerFrameSources(const PlayerFrameSelectionPlan& plan);
absl::Status ValidatePlayerFrameSourceContext(
    const PlayerFrameSelectionPlan& plan,
    const std::string& expected_source_context);

// Boxes must already have passed the real ds-fieldmask plugin and be in canvas coordinates.
// overlap_mask is a bounded CV_8UC1 mask spanning the entire canvas at any resolution.
// This function does not duplicate rink pruning, access images, or perform inference.
absl::StatusOr<PlayerFrameObservation> ScorePlayerFrame(
    const PlayerFramePairIdentity& pair,
    const std::vector<PlayerFrameBox>& boxes,
    const cv::Mat& overlap_mask,
    cv::Size canvas_size);

absl::StatusOr<PlayerFrameSelectionReport> SelectPlayerFrames(
    const PlayerFrameSelectionSettings& settings,
    const std::vector<PlayerFrameObservation>& observations,
    const std::vector<PlayerFrameSourceBinding>& sources,
    const PlayerFrameSelectionContext& context);

absl::StatusOr<std::string> PlayerFrameSelectionFingerprint(const PlayerFrameSelectionPlan& plan);
// Stable hash of dimensions and logical CV_8UC1 row bytes (ignores allocator stride).
absl::StatusOr<std::string> PlayerFrameMaskFingerprint(const cv::Mat& mask);
absl::Status ValidatePlayerFrameSelectionPlan(const PlayerFrameSelectionPlan& plan);
YAML::Node PlayerFrameSelectionPlanYaml(const PlayerFrameSelectionPlan& plan);
absl::StatusOr<PlayerFrameSelectionPlan> ParsePlayerFrameSelectionPlan(const YAML::Node& node);
YAML::Node PlayerFrameSelectionReportYaml(const PlayerFrameSelectionReport& report);
absl::StatusOr<PlayerFrameSelectionReport> ParsePlayerFrameSelectionReport(const YAML::Node& node);
absl::StatusOr<PlayerFrameSelectionReport> LoadPlayerFrameSelectionReport(const std::filesystem::path& path);

// Capture paths must call Observe once for every synchronized pair, starting at
// the unchanged decode anchor. Source IDs/sequences are diagnostic, not replay keys.
class PlayerFrameReplaySelector {
 public:
  static absl::StatusOr<PlayerFrameReplaySelector> Create(const PlayerFrameSelectionPlan& plan);
  absl::StatusOr<bool> Observe(const PlayerFramePairIdentity& actual);
  absl::Status Finish() const;
  size_t selected_count() const {
    return next_;
  }
  bool complete() const {
    return next_ == pairs_.size();
  }

 private:
  explicit PlayerFrameReplaySelector(std::vector<PlayerFramePairIdentity> pairs) : pairs_(std::move(pairs)) {}
  std::vector<PlayerFramePairIdentity> pairs_;
  size_t next_{0};
  std::optional<uint64_t> previous_timeline_ns_;
};

} // namespace hm::stitching
