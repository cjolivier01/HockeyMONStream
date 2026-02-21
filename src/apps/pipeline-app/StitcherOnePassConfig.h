#pragma once

#include "hstream/src/libs/common/pipeline_utils.h"

#include "yaml-cpp/yaml.h"

namespace hm {

struct StitcherSizingConfig {
  bool configure_only{false};
  bool one_pass_mode{false};

  bool allow_runtime_canvas() const {
    return configure_only || one_pass_mode;
  }
};

inline StitcherSizingConfig ParseStitcherSizingConfig(const YAML::Node& pipeline) {
  StitcherSizingConfig config;
  config.configure_only = get_node_value(pipeline, "hmstitcher.configure-only", false);
  config.one_pass_mode = get_node_value(pipeline, "hmstitcher.one-pass-mode", false);
  return config;
}

} // namespace hm
