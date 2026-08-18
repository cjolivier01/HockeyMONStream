#pragma once

#include "yaml-cpp/yaml.h"

#include <string>

namespace hm {

struct StitcherSizingConfig {
  bool configure_only{false};
  bool one_pass_mode{false};

  bool allow_runtime_canvas() const {
    return configure_only || one_pass_mode;
  }
};

inline bool parse_one_pass_bool(const YAML::Node& node, bool default_value) {
  if (!node || node.IsNull()) {
    return default_value;
  }
  try {
    return !!node.as<int>();
  } catch (const YAML::Exception&) {
  }
  try {
    return node.as<bool>();
  } catch (const YAML::Exception&) {
  }
  try {
    const std::string scalar = node.as<std::string>();
    return scalar == "1" || scalar == "true" || scalar == "TRUE";
  } catch (const YAML::Exception&) {
  }
  return default_value;
}

inline StitcherSizingConfig ParseStitcherSizingConfig(const YAML::Node& pipeline) {
  StitcherSizingConfig config;
  if (!pipeline || !pipeline.IsMap()) {
    return config;
  }
  const YAML::Node hmstitcher = pipeline["hmstitcher"];
  if (!hmstitcher || !hmstitcher.IsMap()) {
    return config;
  }
  config.configure_only = parse_one_pass_bool(hmstitcher["configure-only"], false);
  config.one_pass_mode = parse_one_pass_bool(hmstitcher["one-pass-mode"], false);
  return config;
}

inline bool OnePassCalibrationRequired(bool one_pass_mode, bool stitching_configured, bool field_mask_configured) {
  return one_pass_mode && (!stitching_configured || !field_mask_configured);
}

} // namespace hm
