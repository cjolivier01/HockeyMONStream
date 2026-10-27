#pragma once

#include <stdexcept>
#include <yaml-cpp/yaml.h>

struct StitchingExperimentDefaults {
  bool prefer_player_frames;
  int scan_duration_seconds;
  int scan_interval_ms;
  int preview_duration_seconds;
  bool preview_loop;
  bool share_saved_rotation;
};

inline StitchingExperimentDefaults ReadStitchingExperimentDefaults(const YAML::Node& baseline) {
  const auto values = baseline["hstream_ui"]["stitching_experiments"];
  const auto number = [&](const char* key, int lower, int upper) {
    const int value = values[key].as<int>();
    if (value < lower || value > upper)
      throw std::runtime_error(std::string("Invalid stitching experiment baseline setting: ") + key);
    return value;
  };
  return {
      values["prefer_player_frames"].as<bool>(),
      number("scan_duration_seconds", 1, 300),
      number("scan_interval_ms", 1, 60000),
      number("preview_duration_seconds", 2, 120),
      values["preview_loop"].as<bool>(),
      values["share_saved_rotation"].as<bool>(),
  };
}
