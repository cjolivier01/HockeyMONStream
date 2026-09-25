#include "hstream/src/libs/player_analytics/Config.h"
#include "hstream/src/libs/player_analytics/ModelCatalog.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hm::player_analytics {
namespace {

bool Flag(const YAML::Node& node) {
  if (!node || node.IsNull())
    return false;
  if (!node.IsScalar())
    throw std::invalid_argument("enable/draw flag must be a boolean or 0/1");
  const std::string value = node.Scalar();
  if (value == "1")
    return true;
  if (value == "0")
    return false;
  return node.as<bool>();
}

void Enable(const YAML::Node& node, FeatureConfig* config) {
  if (!node || node.IsNull())
    return;
  if (!node.IsMap())
    throw std::invalid_argument("feature must be a mapping");
  config->enabled = Flag(node["enable"]);
}

void ReadFeature(const YAML::Node& node, const char* name, FeatureConfig* config) {
  if (!config->enabled)
    return;
  const bool explicit_model = node["model"] && !node["model"].IsNull();
  if (explicit_model)
    config->model = node["model"].as<std::string>();
  const bool custom = !explicit_model || config->model == "custom";
  if (node["bundle"] && !node["bundle"].IsNull() && (custom || node["bundle"].IsScalar()))
    config->bundle = node["bundle"].as<std::string>();
  if (config->bundle.size() > 4096 || config->bundle.find('\0') != std::string::npos) {
    if (custom)
      throw std::invalid_argument(std::string(name) + ".bundle must be a bounded path");
    config->bundle.clear();
  }
  config->model = explicit_model ? config->model
      : !config->bundle.empty()  ? "custom"
                                 : std::string(DefaultModelId(name));
  if (config->model == "custom") {
    if (config->bundle.empty())
      throw std::invalid_argument(std::string(name) + ".bundle is required for a custom model");
  } else if (!FindModel(name, config->model)) {
    throw std::invalid_argument(std::string(name) + ".model is not a supported model selection: " + config->model);
  }
  if (node["rate-hz"])
    config->rate_hz = node["rate-hz"].as<double>();
  if (!std::isfinite(config->rate_hz) || config->rate_hz <= 0 || config->rate_hz > 240)
    throw std::invalid_argument(std::string(name) + ".rate-hz must be finite and in (0,240]");
  if (node["confidence-threshold"])
    config->confidence_threshold = node["confidence-threshold"].as<float>();
  if (!std::isfinite(config->confidence_threshold) || config->confidence_threshold < 0 ||
      config->confidence_threshold > 1)
    throw std::invalid_argument(std::string(name) + ".confidence-threshold must be in [0,1]");
}

size_t Limit(const YAML::Node& node, const char* key, size_t fallback, size_t maximum) {
  if (!node[key])
    return fallback;
  const int64_t value = node[key].as<int64_t>();
  if (value < 1 || static_cast<uint64_t>(value) > maximum)
    throw std::invalid_argument(std::string(key) + " exceeds its positive hard bound");
  return static_cast<size_t>(value);
}

} // namespace

absl::StatusOr<Config> ParseConfig(const YAML::Node& node) {
  try {
    Config config;
    if (!node || node.IsNull())
      return config;
    if (!node.IsMap())
      return absl::InvalidArgumentError("pipeline.player-analytics must be a mapping");
    Enable(node["pose"], &config.pose);
    Enable(node["jersey"], &config.jersey);
    Enable(node["action"], &config.action);
    config.drawing.pose = Flag(node["draw-pose"]);
    config.drawing.jersey = Flag(node["draw-jerseys"]);
    config.drawing.action = Flag(node["draw-actions"]);
    if (!config.enabled())
      return config;
    ReadFeature(node["pose"], "pose", &config.pose);
    ReadFeature(node["jersey"], "jersey", &config.jersey);
    ReadFeature(node["action"], "action", &config.action);
    if (config.jersey.enabled && node["jersey"]["roi-mode"]) {
      const auto mode = node["jersey"]["roi-mode"].as<std::string>();
      if (mode == "pose")
        config.jersey_roi_mode = JerseyRoiMode::kPose;
      else if (mode != "bbox")
        throw std::invalid_argument("jersey.roi-mode must be bbox or pose");
    }
    if (!config.pose.enabled &&
        (config.action.enabled || (config.jersey.enabled && config.jersey_roi_mode == JerseyRoiMode::kPose)))
      throw std::invalid_argument("action and pose-guided jersey recognition require explicit pose.enable");
    if (config.action.enabled && config.pose.rate_hz < 10)
      throw std::invalid_argument("action recognition requires pose.rate-hz >= 10 for the causal sample profile");
    config.maximum_tracks = Limit(node, "max-tracks", kMaximumTracks, kMaximumTracks);
    config.maximum_due_rois = Limit(node, "max-due-rois", kMaximumDueRois, kMaximumDueRois);
    if (config.jersey.enabled && config.jersey_roi_mode == JerseyRoiMode::kPose && config.maximum_due_rois < 2)
      throw std::invalid_argument("pose-guided jersey recognition requires max-due-rois >= 2");
    config.batch_size = Limit(node, "batch-size", std::min(kMaximumBatch, config.maximum_due_rois), kMaximumBatch);
    if (config.batch_size > config.maximum_due_rois)
      throw std::invalid_argument("batch-size must not exceed max-due-rois");
    config.track_retention_ns = Limit(node, "track-retention-ms", 2000, 60000) * 1000000ULL;
    return config;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("player analytics configuration: ") + error.what());
  }
}

} // namespace hm::player_analytics
