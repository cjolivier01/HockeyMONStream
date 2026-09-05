#pragma once

#include <map>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "hstream/src/libs/common/pipeline_utils.h"

namespace hm::pipeline_internal {

inline bool looks_like_pipeline_asset_root(const YAML::Node& config) {
  if (!config || !config.IsMap())
    return false;
  const YAML::Node pipeline = config["pipeline"];
  if (pipeline && pipeline.IsMap())
    return true;
  for (const char* marker :
       {"application", "primary-gie", "secondary-gie0", "source0", "sink0", "hmstitcher", "hmplaycropper"}) {
    if (config[marker].IsDefined())
      return true;
  }
  return false;
}

inline YAML::Node pipeline_asset_root(YAML::Node config) {
  const YAML::Node pipeline = config && config.IsMap() ? config["pipeline"] : YAML::Node();
  if (pipeline && pipeline.IsMap())
    return pipeline;
  return config;
}

inline void apply_pipeline_options_for_asset_discovery(
    YAML::Node config,
    const std::vector<std::map<std::string, std::string>>& option_sets) {
  if (!looks_like_pipeline_asset_root(config))
    return;
  const YAML::Node pipeline = config["pipeline"];
  const bool nested_pipeline_root = pipeline && pipeline.IsMap();
  for (const auto& options : option_sets) {
    for (const auto& item : options) {
      std::string key = item.first;
      if (!nested_pipeline_root && key.rfind("pipeline.", 0) == 0)
        key = key.substr(std::string("pipeline.").size());
      config = hm::set_node_value(config, key, item.second);
    }
  }
}

} // namespace hm::pipeline_internal
