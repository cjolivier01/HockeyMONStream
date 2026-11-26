#pragma once

#include <optional>
#include "hstream/src/libs/draw_display/RinkMiniMap.h"
#include "hstream/src/libs/gpu/Image.h"
#include "hstream/src/libs/scoreboard/ScoreboardPlan.h"

namespace hm::program {
// Resolved rendering controls, shared independently of any GPU SDK.
struct ProgramRenderSettings {
  bool no_crop;
  float fixed_edge_rotation_angle_left, fixed_edge_rotation_angle_right;
  float shadow_lift_percent;
  bool lift_shadow_black_point;
  float exposure;
  std::optional<scoreboard::Plan> scoreboard;
  std::shared_ptr<const draw_display::RinkMapFrame> rink_map;
  std::shared_ptr<const gpu::Image> scoreboard_source;
};
} // namespace hm::program
