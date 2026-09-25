#include "hstream/src/libs/draw_display/PlayerOverlays.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

#include "hstream/src/libs/player_analytics/FrameMeta.h"
#include "hstream/src/libs/player_analytics/TrackColors.h"

namespace hm::draw_display::analytics {
namespace {
using preview_overlay::Point;
constexpr std::array<std::array<unsigned, 2>, 19> kBones{{
    {15, 13}, {13, 11}, {16, 14}, {14, 12}, {11, 12}, {5, 11}, {6, 12}, {5, 6}, {5, 7}, {6, 8},
    {7, 9},   {8, 10},  {1, 2},   {0, 1},   {0, 2},   {1, 3},  {2, 4},  {3, 5}, {4, 6},
}};

Color PlayerColor(uint8_t slot) {
  if (slot >= player_analytics::kTrackPalette.size())
    return {0.45F, 0.45F, 0.45F, 1};
  const auto& color = player_analytics::kTrackPalette[slot];
  return {color.red, color.green, color.blue, color.alpha};
}
bool VisibleBox(const std::array<Point, 4>& corners, float width, float height) {
  for (const auto& point : corners)
    if (!std::isfinite(point.x) || !std::isfinite(point.y))
      return false;
  // Separating axes for the transformed rectangle and the viewport. Testing
  // only axis-aligned bounds admits rotated players beyond a crop corner.
  const auto separated = [&](float x, float y) {
    float low = corners[0].x * x + corners[0].y * y, high = low;
    for (const auto& point : corners) {
      const float projection = point.x * x + point.y * y;
      low = std::min(low, projection);
      high = std::max(high, projection);
    }
    const float viewport_low = std::min(0.0F, width * x) + std::min(0.0F, height * y);
    const float viewport_high = std::max(0.0F, width * x) + std::max(0.0F, height * y);
    return high <= viewport_low || low >= viewport_high;
  };
  if (separated(1, 0) || separated(0, 1))
    return false;
  for (size_t i = 0; i < 2; ++i) {
    const float dx = corners[i + 1].x - corners[i].x, dy = corners[i + 1].y - corners[i].y;
    if ((dx == 0 && dy == 0) || separated(-dy, dx))
      return false;
  }
  return true;
}
} // namespace

void BuildPlayerOverlays(
    const NvDsFrameMeta* frame,
    uint32_t layers,
    float joint_confidence,
    const preview_overlay::PlayCropperTransform* transform,
    float coordinate_width,
    float coordinate_height,
    CommandList* commands) {
  if (transform)
    layers &= ~transform->baked_player_layers;
  if (!layers || !frame || !commands || !std::isfinite(coordinate_width) || !std::isfinite(coordinate_height) ||
      coordinate_width <= 0 || coordinate_height <= 0)
    return;
  const auto* result = player_analytics::FindFrameResult(frame);
  if (result && (result->pts_ns != frame->buf_pts || result->stream_id != frame->source_id))
    result = nullptr;
  if (result)
    layers &= ~result->baked_layers;
  if (!layers)
    return;
  const auto map = [transform](Point point) {
    return transform ? preview_overlay::metadata_to_output(*transform, point) : point;
  };
  const float scale = std::clamp(coordinate_height / 1080.0F, 0.5F, 8.0F);
  const float line_width = 2 * scale;
  if (layers & kPlayerBoxes) {
    // Program calls before object metadata is transformed. Preview boxes use
    // the existing immutable overlay snapshot and its independent UI switch.
    for (const auto* item = frame->obj_meta_list; item; item = item->next) {
      const auto* object = static_cast<const NvDsObjectMeta*>(item->data);
      if (!object || object->class_id != 0 || object->object_id == player_analytics::kUntrackedId)
        continue;
      const auto& rect = object->rect_params;
      const std::array<Point, 4> points{
          {map({rect.left, rect.top}),
           map({rect.left + rect.width, rect.top}),
           map({rect.left + rect.width, rect.top + rect.height}),
           map({rect.left, rect.top + rect.height})}};
      const auto& c = rect.border_color;
      const Color color{
          static_cast<float>(c.red),
          static_cast<float>(c.green),
          static_cast<float>(c.blue),
          static_cast<float>(c.alpha)};
      for (size_t i = 0; i < points.size(); ++i) {
        const auto& a = points[i];
        const auto& b = points[(i + 1) % points.size()];
        commands->AddLine(a.x, a.y, b.x, b.y, line_width, color);
      }
    }
  }
  if (!(layers & (kPose | kJerseys | kActions)))
    return;
  if (!result)
    return;
  // Pose first across all players so bounded text cannot starve geometry.
  if (layers & kPose) {
    for (size_t i = 0; i < result->player_count; ++i) {
      const auto& player = result->players[i];
      if (!player.has_pose || player.pose_observed_at != result->pts_ns)
        continue;
      const Color color = PlayerColor(player.color_slot);
      for (const auto& bone : kBones) {
        const auto& a = player.pose[bone[0]];
        const auto& b = player.pose[bone[1]];
        if (a.confidence <= 0 || b.confidence <= 0 || a.confidence < joint_confidence ||
            b.confidence < joint_confidence)
          continue;
        const Point start = map({a.x, a.y}), end = map({b.x, b.y});
        commands->AddLine(start.x, start.y, end.x, end.y, line_width, color);
      }
      for (const auto& joint : player.pose) {
        if (joint.confidence <= 0 || joint.confidence < joint_confidence)
          continue;
        const Point point = map({joint.x, joint.y});
        commands->AddDisc(point.x, point.y, 2.5F * scale, color);
      }
    }
  }
  if (!(layers & (kJerseys | kActions)))
    return;
  for (size_t i = 0; i < result->player_count; ++i) {
    const auto& player = result->players[i];
    const Color color = PlayerColor(player.color_slot);
    const auto& box = player.box;
    const std::array<Point, 4> corners{
        {map({box.left, box.top}),
         map({box.left + box.width, box.top}),
         map({box.left + box.width, box.top + box.height}),
         map({box.left, box.top + box.height})}};
    if (!VisibleBox(corners, coordinate_width, coordinate_height))
      continue;
    float x = corners[0].x, y = corners[0].y;
    for (const auto& point : corners) {
      x = std::min(x, point.x);
      y = std::min(y, point.y);
    }
    const float height = 24 * scale;
    x = std::clamp(x, 0.0F, coordinate_width);
    y = std::max(height, y) - height;
    const auto label = [&](std::string_view text) {
      if (text.empty())
        return;
      // Keep label backgrounds bounded with the text; a rejected label must
      // not leave a standalone opaque rectangle over the picture.
      commands->AddText(x, y, height, text, color);
      y += height;
    };
    if ((layers & kJerseys) && player.jersey.text[0] && player.jersey.expires_at >= result->pts_ns)
      label(player.jersey.text.data());
    if ((layers & kActions) && player.action.label >= 0 && player.action.expires_at >= result->pts_ns)
      label(player.action_text.data());
  }
}

} // namespace hm::draw_display::analytics
