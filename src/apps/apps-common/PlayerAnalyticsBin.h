#pragma once

#include <string>

#include <gst/gst.h>
#include <yaml-cpp/yaml.h>

#include "absl/status/statusor.h"
#include "hstream/src/libs/player_analytics/Config.h"

namespace hm::gst {

struct PlayerAnalyticsConfig {
  player_analytics::Config analytics;
  std::string serialized;
  unsigned gpu_id{0};
};

// Pure CPU resolution after layer/mode selection. Disabled children never
// resolve paths and cannot cause a plugin, model or CUDA context to be loaded.
absl::StatusOr<PlayerAnalyticsConfig> ResolvePlayerAnalyticsConfig(
    const YAML::Node& pipeline,
    const std::string& config_dir,
    int default_gpu_id);

// Returns a null element when disabled. Otherwise the caller owns a floating
// reference and adds it after the native tracker, before the play tracker.
absl::StatusOr<GstElement*> CreatePlayerAnalytics(const PlayerAnalyticsConfig& config, bool native_tracker_enabled);

} // namespace hm::gst
