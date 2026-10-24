#pragma once

namespace hm::program {
// Resolved rendering controls, shared independently of any GPU SDK.
struct ProgramRenderSettings {
  bool no_crop;
  float fixed_edge_rotation_angle_left, fixed_edge_rotation_angle_right;
  float shadow_lift_percent;
  bool lift_shadow_black_point;
  float exposure;
};
} // namespace hm::program
