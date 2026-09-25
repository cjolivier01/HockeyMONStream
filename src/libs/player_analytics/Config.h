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

  bool enabled() const {
    return pose.enabled || jersey.enabled || action.enabled;
  }
};

// Pass the final resolved pipeline.player-analytics node, not a config path.
// Pure parsing: never opens bundle paths. Disabled features inspect only enable;
// absent/all-disabled inference also skips all inference-limit validation.
// Draw preferences cannot enable inference or satisfy a pose dependency.
absl::StatusOr<Config> ParseConfig(const YAML::Node& node);

} // namespace hm::player_analytics
