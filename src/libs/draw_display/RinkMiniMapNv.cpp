#include "hstream/src/libs/common/PreviewOverlayMeta.h"
#include "hstream/src/libs/draw_display/RinkMiniMap.h"
#include "hstream/src/libs/player_analytics/FrameMeta.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

namespace hm::draw_display {
RinkMapFrame ReadRinkMapFrame(const NvDsFrameMeta* frame, unsigned width, unsigned height) {
  RinkMapFrame out;
  if (!frame)
    return out;
  out.stitched_width = width;
  out.stitched_height = height;
  out.pts = frame->buf_pts;
  out.sequence = frame->frame_num;
  out.source_id = frame->source_id;
  if (const auto* generation = stitching::find_stitched_output_generation_meta(frame))
    out.generation = generation->generation();
  if (const auto* snapshot = preview_overlay::find_overlay_snapshot_meta(frame)) {
    out.have_tracks = true;
    out.total_players = snapshot->player_rects.size();
    out.coordinate_width = snapshot->coordinate_width;
    out.coordinate_height = snapshot->coordinate_height;
    for (const auto& player : snapshot->player_rects) {
      if (out.players.size() == player_analytics::kMaximumTracks)
        break;
      const auto& r = player.rect;
      const auto& c = r.border_color;
      out.players.push_back(
          {r.left,
           r.top,
           r.width,
           r.height,
           {float(c.red), float(c.green), float(c.blue), float(c.alpha)},
           player.track_id,
           player.ignored});
    }
  }
  out.analytics = player_analytics::ShareFrameResult(frame);
  return out;
}
bool RinkMiniMap::Append(
    const NvDsFrameMeta* frame,
    unsigned width,
    unsigned height,
    float display_width,
    float display_height,
    analytics::CommandList* commands) {
  return Append(ReadRinkMapFrame(frame, width, height), display_width, display_height, commands);
}
} // namespace hm::draw_display
