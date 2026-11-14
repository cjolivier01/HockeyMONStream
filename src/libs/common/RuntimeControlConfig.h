#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "yaml-cpp/yaml.h"

namespace hm::config {
struct RuntimeControlKey {
  const char* id;
  const char* path;
  int divisor;
  bool boolean;
};

// IDs and integer scaling belong to presentation; YAML stores native units.
inline constexpr std::array<RuntimeControlKey, 9> kRuntimeControlKeys{{
    {"Apply_To_Fast_Box", "rink.camera.motion_overrides.apply_to_fast_box", 1, true},
    {"Apply_To_Follower_Box", "rink.camera.motion_overrides.apply_to_follower_box", 1, true},
    {"Max_Speed_X_x10", "rink.camera.motion_overrides.max_speed_x", 10, false},
    {"Max_Speed_Y_x10", "rink.camera.motion_overrides.max_speed_y", 10, false},
    {"Max_Accel_X_x10", "rink.camera.motion_overrides.max_accel_x", 10, false},
    {"Max_Accel_Y_x10", "rink.camera.motion_overrides.max_accel_y", 10, false},
    {"Bring_Up_Shadows", "video_out.tone.shadow_lift_percent", 1, false},
    {"Lift_Shadow_Black_Point", "video_out.tone.lift_shadow_black_point", 1, true},
    {"Exposure_x100", "video_out.tone.exposure", 100, false},
}};

// Normalize one layer before merging, so a legacy game key can override a
// canonical user default. Canonical keys win within a layer. Input is immutable.
// Throws on invalid legacy values, like the YAML parser used by its callers.
YAML::Node NormalizeRuntimeControlLayer(const YAML::Node& layer);
// Preserve existing YAML handles when merging mutable runtime configuration.
void NormalizeRuntimeControlLayerInPlace(YAML::Node layer);
// Required canonical playback origin; accepts legacy aliases in sparse snapshots.
int64_t PlaybackStartNanoseconds(const YAML::Node& config);
std::string PlaybackStartTime(const YAML::Node& config);
double RuntimeControlPresentationValue(const YAML::Node& config, const std::string& id);
} // namespace hm::config
