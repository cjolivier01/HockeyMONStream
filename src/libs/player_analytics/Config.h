#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include <yaml-cpp/yaml.h>

#include "absl/status/statusor.h"
#include "hstream/src/libs/player_analytics/Types.h"

namespace hm::player_analytics {

enum class JerseyRoiMode { kBox, kPose };

// Canonical baseline keys under `team:` paired with the native keys they map
// to under `pipeline.player-analytics.team`. One table so the configurator and
// the UI's validation preview cannot disagree about what the baseline owns;
// the two apply different precedence rules to the same set of names.
inline constexpr std::array<std::pair<const char*, const char*>, 5> kTeamCanonicalKeys{{
    {"warmup_samples", "warmup-samples"},
    {"refit_interval", "refit-interval"},
    {"min_margin", "min-margin"},
    {"rate_hz", "rate-hz"},
    {"model", "model"},
}};

struct FeatureConfig {
  bool enabled{false};
  std::string bundle;
  double rate_hz{1};
  float confidence_threshold{0.5F};
  // "custom" selects the explicit directory; otherwise this is a catalog ID.
  // The runner resolves built-ins into the internal directory before playback.
  std::string model;
  // Pose's "fps" cadence follows incoming frames without timestamp rounding.
  bool every_frame{false};
};

struct DrawingConfig {
  bool pose{false};
  bool jersey{false};
  bool action{false};
  bool team{false};
};

struct Config {
  FeatureConfig pose{false, {}, 10, 0.3F};
  FeatureConfig jersey{false, {}, 2, 0.8F};
  FeatureConfig action{false, {}, 1, 0.5F};
  // No rate or threshold literal here, unlike the three above: ParseConfig
  // refuses an enabled team feature that did not bring rate-hz from the
  // baseline, and rejects confidence-threshold outright, so a value here would
  // be a second unreachable statement of the shipped default.
  FeatureConfig team;
  // Two-means tuning, supplied entirely by configs/baseline.yaml.
  TeamClusteringSettings team_clustering;
  JerseyRoiMode jersey_roi_mode{JerseyRoiMode::kBox};
  // Which sweater the home side wore. Unset leaves the overlay reading
  // DARK/LIGHT, which is correct but unnamed, rather than guessing a side.
  std::optional<bool> home_is_dark;
  DrawingConfig drawing;
  bool continuous_pose_drawing{true};
  size_t maximum_tracks{kMaximumTracks};
  size_t maximum_due_rois{kMaximumDueRois};
  size_t batch_size{kMaximumBatch};
  uint64_t track_retention_ns{2 * kSecond};

  uint32_t drawing_layers() const {
    return (pose.enabled && drawing.pose ? kDrawPose : 0U) | (jersey.enabled && drawing.jersey ? kDrawJerseys : 0U) |
        (action.enabled && drawing.action ? kDrawActions : 0U) |
        (team.enabled && drawing.team ? kDrawTeams : 0U);
  }

  bool enabled() const {
    return pose.enabled || jersey.enabled || action.enabled || team.enabled;
  }
};

// Pass the final resolved pipeline.player-analytics node, not a config path.
// Pure parsing: never opens model paths. Disabled features inspect only enable;
// absent/all-disabled inference also skips all inference-limit validation.
// Draw preferences cannot enable inference or satisfy a pose dependency.
absl::StatusOr<Config> ParseConfig(const YAML::Node& node);

} // namespace hm::player_analytics
