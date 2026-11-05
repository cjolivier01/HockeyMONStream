#pragma once

#include <nvdsmeta.h>
#include <cstdint>
#include <string_view>

#include "hstream/src/libs/common/PreviewOverlayMeta.h"
#include "hstream/src/libs/draw_display/AnalyticsOverlay.h"
#include "hstream/src/libs/player_analytics/Types.h"

namespace hm::draw_display::analytics {

enum PlayerLayer : uint32_t {
  kPlayerBoxes = player_analytics::kDrawPlayerBoxes,
  kPose = player_analytics::kDrawPose,
  kJerseys = player_analytics::kDrawJerseys,
  kActions = player_analytics::kDrawActions,
  kTeams = player_analytics::kDrawTeams
};

// Shared by Program's CUDA overlay and the GPU preview's box diagnostics.
// A role takes the row otherwise used by IGNORED. Ordinary players have no
// label. Text is upright above the transformed box, and omitted if off-crop.
void AddPlayerBoxLabel(
    const NvOSD_RectParams& rect,
    std::string_view role,
    bool ignored,
    const preview_overlay::PlayCropperTransform* transform,
    float coordinate_width,
    float coordinate_height,
    CommandList* commands);

// CPU metadata only. Caller skips this function when layers==0. A Program
// transform maps original metadata pixels to owned output pixels; null means
// Stitched coordinates. Baked layers are excluded before metadata traversal.
// Clear/reuse commands at the caller, so storage persists across frames.
void BuildPlayerOverlays(
    const NvDsFrameMeta* frame,
    uint32_t layers,
    float joint_confidence,
    const preview_overlay::PlayCropperTransform* transform,
    float coordinate_width,
    float coordinate_height,
    CommandList* commands);

} // namespace hm::draw_display::analytics
