#include "hstream/src/libs/draw_display/RinkMiniMap.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "hstream/src/libs/common/PreviewOverlayMeta.h"
#include "hstream/src/libs/player_analytics/FrameMeta.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

namespace hm::draw_display {
bool RinkMiniMap::Append(
    const NvDsFrameMeta* frame,
    unsigned stitched_width,
    unsigned stitched_height,
    float display_width,
    float display_height,
    analytics::CommandList* commands) {
  if (!map_ || !frame || !commands || !std::isfinite(display_width) || !std::isfinite(display_height) ||
      display_width <= 0 || display_height <= 0)
    return false;
  const auto* generation = stitching::find_stitched_output_generation_meta(frame);
  const auto* snapshot = preview_overlay::find_overlay_snapshot_meta(frame);
  if (!generation || !snapshot || !std::isfinite(snapshot->coordinate_width) ||
      !std::isfinite(snapshot->coordinate_height) || snapshot->coordinate_width <= 0 ||
      snapshot->coordinate_height <= 0)
    return false;
  if (generation->generation() != output_generation_ || width_ != stitched_width || height_ != stitched_height) {
    output_generation_ = generation->generation();
    width_ = stitched_width;
    height_ = stitched_height;
    rotation_ = map_->OutputRotation(output_generation_, width_, height_);
  }
  if (!rotation_)
    return false;
  using analytics::Color;
  constexpr Color ink{0.10F, 0.15F, 0.22F, 1}, white{0.96F, 0.98F, 1, 1};
  constexpr Color red{0.86F, 0.16F, 0.23F, 1}, blue{0.12F, 0.39F, 0.86F, 1};
  const auto& rink = map_->layout;
  const float rink_width = std::min(display_width * 0.29F, float(display_height * 0.25 * rink.length / rink.width));
  const float scale = rink_width / 400.0F;
  const float rink_height = rink_width * rink.width / rink.length;
  const float margin = 12 * scale, header = 24 * scale, footer = 20 * scale;
  // Keep the lower-right corner free for the existing preview watermark.
  const float left = 2 * margin;
  const float top = display_height - rink_height - header - footer - 2 * margin;
  if (!commands->AddFill(
          left - margin, top, left + rink_width + margin, display_height - margin, {0.96F, 0.98F, 1, 0.90F}))
    return false;
  const auto point = [&](cv::Point2d p) {
    return cv::Point2f(
        left + float(p.x / rink.length + 0.5) * rink_width, top + header + float(0.5 - p.y / rink.width) * rink_height);
  };
  commands->AddText(left, top + 3 * scale, 16 * scale, "RINK MAP - ESTIMATED", ink);
  const auto lines = stitching::RinkLayoutDrawing(rink);
  for (size_t i = 0; i < lines.size(); ++i) {
    const Color color = i == 0 ? ink : (i == 2 || i == 4) ? blue : red;
    for (size_t j = 1; j < lines[i].size(); ++j) {
      const auto a = point(lines[i][j - 1]), b = point(lines[i][j]);
      commands->AddLine(a.x, a.y, b.x, b.y, 1.5F * scale, color);
    }
  }
  // Conventional centre circle; its diameter is a drawing cue, not an extra
  // observation or constraint on the saved map.
  for (int i = 0; i < 32; ++i) {
    const double a = i * 2 * 3.14159265358979323846 / 32;
    const double b = (i + 1) * 2 * 3.14159265358979323846 / 32;
    const auto p = point({4.572 * std::cos(a), 4.572 * std::sin(a)});
    const auto q = point({4.572 * std::cos(b), 4.572 * std::sin(b)});
    commands->AddLine(p.x, p.y, q.x, q.y, scale, blue);
  }
  for (double x : {-rink.length / 2 + rink.goal_inset, rink.length / 2 - rink.goal_inset}) {
    const auto p = point({x, 0});
    commands->AddRectangle(p.x - 3 * scale, p.y - 7 * scale, p.x + 3 * scale, p.y + 7 * scale, 1.5F * scale, red);
  }
  const auto* result = player_analytics::FindFrameResult(frame);
  if (result && (result->pts_ns != frame->buf_pts || result->stream_id != frame->source_id))
    result = nullptr;
  size_t visible = 0;
  // Track colours do not encode teams. All dots are neutral until there is an
  // explicit team classification; jersey numbers alone cannot supply one.
  for (const auto& player : snapshot->player_rects) {
    if (visible >= player_analytics::kMaximumTracks)
      break;
    const auto& box = player.rect;
    if (!std::isfinite(box.width) || !std::isfinite(box.height) || box.width <= 0 || box.height <= 0)
      continue;
    auto location = map_->Locate(
        (box.left + box.width / 2) / snapshot->coordinate_width,
        (box.top + box.height) / snapshot->coordinate_height,
        *rotation_,
        stitched_width,
        stitched_height);
    if (!location)
      continue;
    const auto p = point(*location);
    std::string_view number;
    if (result && player.track_id != player_analytics::kUntrackedId) {
      for (size_t i = 0; i < std::min(result->player_count, player_analytics::kMaximumTracks); ++i) {
        const auto& value = result->players[i];
        const auto& jersey = value.jersey;
        if (value.track_id != player.track_id || jersey.observed_at > frame->buf_pts ||
            jersey.expires_at < frame->buf_pts)
          continue;
        size_t length = 0;
        while (length < 2 && jersey.text[length] >= '0' && jersey.text[length] <= '9')
          ++length;
        if (length && jersey.text[length] == '\0')
          number = std::string_view(jersey.text.data(), length);
        break;
      }
    }
    const float radius = (number.empty() ? 5.0F : 10.0F) * scale;
    commands->AddDisc(p.x, p.y, radius + 1.5F * scale, white);
    commands->AddDisc(p.x, p.y, radius, ink);
    if (!number.empty()) {
      const float text_height = 13 * scale;
      commands->AddText(p.x - number.size() * text_height / 3, p.y - text_height / 2, text_height, number, white);
    }
    ++visible;
  }
  commands->AddText(
      left, top + header + rink_height + 4 * scale, 13 * scale, visible ? "TEAMS UNKNOWN" : "NO PLAYERS LOCATED", ink);
  return true;
}
} // namespace hm::draw_display
