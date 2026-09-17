#include "StitcherOnePassConfig.h"

#include <iostream>

int main() {
  YAML::Node pipeline;

  auto cfg = hm::ParseStitcherSizingConfig(pipeline);
  if (cfg.allow_runtime_canvas()) {
    std::cerr << "Expected runtime canvas sizing to be disabled by default" << std::endl;
    return 1;
  }

  pipeline["hmstitcher"]["configure-only"] = "1";
  cfg = hm::ParseStitcherSizingConfig(pipeline);
  if (!cfg.configure_only || cfg.one_pass_mode || !cfg.allow_runtime_canvas()) {
    std::cerr << "configure-only should enable runtime canvas sizing without one-pass" << std::endl;
    return 1;
  }

  pipeline["hmstitcher"]["configure-only"] = "0";
  pipeline["hmstitcher"]["one-pass-mode"] = "1";
  cfg = hm::ParseStitcherSizingConfig(pipeline);
  if (!cfg.one_pass_mode || cfg.configure_only || !cfg.allow_runtime_canvas()) {
    std::cerr << "one-pass-mode should enable runtime canvas sizing when set" << std::endl;
    return 1;
  }

  return 0;
}
