#include "hstream/src/apps/apps-common/PlayerAnalyticsBin.h"

#include <algorithm>
#include <filesystem>

namespace hm::gst {

absl::StatusOr<PlayerAnalyticsConfig> ResolvePlayerAnalyticsConfig(
    const YAML::Node& pipeline,
    const std::string& config_dir,
    int default_gpu_id) {
  PlayerAnalyticsConfig result;
  try {
    const YAML::Node node = pipeline["player-analytics"];
    const auto parsed = player_analytics::ParseConfig(node);
    if (!parsed.ok())
      return parsed.status();
    result.analytics = *parsed;
    if (!result.analytics.enabled())
      return result;
    const int gpu_id = node["gpu-id"] ? node["gpu-id"].as<int>() : std::max(0, default_gpu_id);
    if (gpu_id < 0)
      return absl::InvalidArgumentError("player-analytics.gpu-id must be nonnegative");
    result.gpu_id = static_cast<unsigned>(gpu_id);
    YAML::Node resolved = YAML::Clone(node);
    for (const char* feature : {"pose", "jersey", "action"}) {
      const bool enabled = std::string(feature) == "pose" ? result.analytics.pose.enabled
          : std::string(feature) == "jersey"              ? result.analytics.jersey.enabled
                                                          : result.analytics.action.enabled;
      if (!enabled)
        continue;
      std::filesystem::path bundle(resolved[feature]["bundle"].as<std::string>());
      if (bundle.is_relative())
        bundle = std::filesystem::absolute(std::filesystem::path(config_dir) / bundle);
      const std::string path = bundle.lexically_normal().string();
      resolved[feature]["bundle"] = path;
      if (std::string(feature) == "pose")
        result.analytics.pose.bundle = path;
      else if (std::string(feature) == "jersey")
        result.analytics.jersey.bundle = path;
      else
        result.analytics.action.bundle = path;
    }
    result.serialized = YAML::Dump(resolved);
    return result;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("player analytics pipeline settings: ") + error.what());
  }
}

absl::StatusOr<GstElement*> CreatePlayerAnalytics(const PlayerAnalyticsConfig& config, bool native_tracker_enabled) {
  if (!config.analytics.enabled())
    return static_cast<GstElement*>(nullptr);
  if (!native_tracker_enabled)
    return absl::FailedPreconditionError("player analytics requires pipeline.tracker.enable=1");
  GstElement* element = gst_element_factory_make("hmplayeranalytics", "player_analytics");
  if (!element)
    return absl::NotFoundError("The enabled hmplayeranalytics plugin is not installed or could not be loaded");
  g_object_set(element, "configuration", config.serialized.c_str(), "gpu-id", config.gpu_id, nullptr);
  return element;
}

} // namespace hm::gst
