#pragma once

#include <cstddef>
#include <string>

#include <yaml-cpp/yaml.h>

#include "absl/status/statusor.h"
#include "hstream/src/libs/player_analytics/Types.h"

namespace hm::player_analytics {

enum class JerseyRoiMode { kBox, kPose };

struct FeatureConfig {
  bool enabled{false};
  std::string bundle;
  double rate_hz{1};
  float confidence_threshold{0.5F};
  // "custom" selects the explicit directory; otherwise this is a catalog ID.
  // The runner resolves built-ins into the internal directory before playback.
  std::string model;
};

struct DrawingConfig {
  bool pose{false};
  bool jersey{false};
  bool action{false};
};

struct Config {
  FeatureConfig pose{false, {}, 10, 0.3F};
  FeatureConfig jersey{false, {}, 2, 0.8F};
  FeatureConfig action{false, {}, 1, 0.5F};
  JerseyRoiMode jersey_roi_mode{JerseyRoiMode::kBox};
  DrawingConfig drawing;
  size_t maximum_tracks{kMaximumTracks};
  size_t maximum_due_rois{kMaximumDueRois};
  size_t batch_size{kMaximumBatch};
  uint64_t track_retention_ns{2 * kSecond};

  uint32_t drawing_layers() const {
    return (pose.enabled && drawing.pose ? kDrawPose : 0U) | (jersey.enabled && drawing.jersey ? kDrawJerseys : 0U) |
        (action.enabled && drawing.action ? kDrawActions : 0U);
  }

  bool enabled() const {
    return pose.enabled || jersey.enabled || action.enabled;
  }
};

// Pass the final resolved pipeline.player-analytics node, not a config path.
// Pure parsing: never opens model paths. Disabled features inspect only enable;
// absent/all-disabled inference also skips all inference-limit validation.
// Draw preferences cannot enable inference or satisfy a pose dependency.
absl::StatusOr<Config> ParseConfig(const YAML::Node& node);

} // namespace hm::player_analytics
