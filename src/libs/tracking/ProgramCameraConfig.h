#pragma once

#include <string>
#include "hockeymon/csrc/play_tracker/PlayTracker.h"
#include "yaml-cpp/yaml.h"

// Shared by the DeepStream adapter and the native AMD pipeline. Preserve the
// legacy namespace so existing callers use exactly the same configuration path.
namespace gst_hm_playtracker {
hm::BBox program_camera_arena(const hm::BBox& frame, const hm::BBox& field);
bool supported_camera(const std::string& name);
bool validate_numeric_yaml_fields(const YAML::Node& node, std::string* error);
hm::play_tracker::PlayTrackerConfig create_play_tracker_config(const hm::BBox& arena_box, const YAML::Node& yaml);
} // namespace gst_hm_playtracker
