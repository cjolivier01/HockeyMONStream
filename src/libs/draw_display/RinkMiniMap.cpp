#include "hstream/src/libs/draw_display/RinkMiniMap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>

#include "hstream/src/libs/common/PreviewOverlayMeta.h"
#include "hstream/src/libs/player_analytics/FrameMeta.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

namespace hm::draw_display {
RinkMiniMap::RinkMiniMap(std::shared_ptr<const stitching::PlaybackRinkMap> map, RinkMiniMapOptions options)
    : map_(std::move(map)), options_(options) {
  if (!map_)
    return;
  layout_lines_ = stitching::RinkLayoutDrawing(map_->layout);
  // Static geometry is prepared once; playback only scales cached points.
  for (size_t i = 0; i < centre_circle_.size(); ++i) {
    const double angle = i * 2 * 3.14159265358979323846 / (centre_circle_.size() - 1);
    centre_circle_[i] = {4.572 * std::cos(angle), 4.572 * std::sin(angle)};
  }
}

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
      snapshot->coordinate_height <= 0) {
    smoothing_.Reset();
    last_pts_.reset();
    return false;
  }
  if (generation->generation() != output_generation_ || width_ != stitched_width || height_ != stitched_height) {
    output_generation_ = generation->generation();
    width_ = stitched_width;
    height_ = stitched_height;
    rotation_ = map_->OutputRotation(output_generation_, width_, height_);
    smoothing_.Reset();
  }
  if (!rotation_)
    return false;
  if (frame->source_id != source_id_) {
    // Another stream's teams are not this one's, named or not.
    named_sides_ = false;
    analytics_epoch_.reset();
  }
  if (!last_pts_ || frame->source_id != source_id_ || frame->buf_pts < *last_pts_ ||
      frame->buf_pts == player_analytics::kInvalidTime || frame->buf_pts - *last_pts_ > 300000000 ||
      frame->frame_num < last_frame_num_)
    smoothing_.Reset();
  last_pts_ = frame->buf_pts;
  last_frame_num_ = frame->frame_num;
  source_id_ = frame->source_id;
  using analytics::Color;
  constexpr Color ink{0.10F, 0.15F, 0.22F, 1}, white{0.96F, 0.98F, 1, 1};
  constexpr Color red{0.86F, 0.16F, 0.23F, 1}, blue{0.12F, 0.39F, 0.86F, 1};
  const auto& rink = map_->layout;
  const float factor = options_.size == RinkMiniMapOptions::Size::kIcon ? .375F
      : options_.size == RinkMiniMapOptions::Size::kCompact             ? .75F
      : options_.size == RinkMiniMapOptions::Size::kLarge               ? 1.5F
                                                                        : 1.0F;
  // Fit the complete panel and outer margins, including on short panoramas.
  const float rink_width = std::min(
      {display_width * .29F * factor,
       float(display_height * .25 * factor * rink.length / rink.width),
       display_width * 400 / 448,
       float(display_height * 400 / (400 * rink.width / rink.length + 80))});
  const float scale = rink_width / 400.0F;
  const float rink_height = rink_width * rink.width / rink.length;
  const float margin = 12 * scale, header = 24 * scale, footer = 20 * scale;
  // Keep the lower-right corner free for the existing preview watermark.
  const float left = options_.position == RinkMiniMapOptions::Position::kUpperRight
      ? display_width - rink_width - 2 * margin
      : 2 * margin;
  const float top = options_.position == RinkMiniMapOptions::Position::kLowerLeft
      ? display_height - rink_height - header - footer - 2 * margin
      : margin;
  const float bottom = top + rink_height + header + footer + margin;
  if (!commands->AddFill(left - margin, top, left + rink_width + margin, bottom, {0.96F, 0.98F, 1, 0.90F}))
    return false;
  const auto point = [&](cv::Point2d p) {
    return cv::Point2f(
        left + float(p.x / rink.length + 0.5) * rink_width, top + header + float(0.5 - p.y / rink.width) * rink_height);
  };
  commands->AddText(left, top + 3 * scale, 16 * scale, "RINK MAP - ESTIMATED", ink);
  for (size_t i = 0; i < layout_lines_.size(); ++i) {
    const Color color = i == 0 ? ink : (i == 2 || i == 4) ? blue : red;
    for (size_t j = 1; j < layout_lines_[i].size(); ++j) {
      const auto a = point(layout_lines_[i][j - 1]), b = point(layout_lines_[i][j]);
      commands->AddLine(a.x, a.y, b.x, b.y, 1.5F * scale, color);
    }
  }
  // Conventional centre circle; its diameter is a drawing cue, not an extra
  // observation or constraint on the saved map.
  for (size_t i = 1; i < centre_circle_.size(); ++i) {
    const auto p = point(centre_circle_[i - 1]);
    const auto q = point(centre_circle_[i]);
    commands->AddLine(p.x, p.y, q.x, q.y, scale, blue);
  }
  for (double x : {-rink.length / 2 + rink.goal_inset, rink.length / 2 - rink.goal_inset}) {
    const auto p = point({x, 0});
    commands->AddRectangle(p.x - 3 * scale, p.y - 7 * scale, p.x + 3 * scale, p.y + 7 * scale, 1.5F * scale, red);
  }
  const auto* result = player_analytics::FindFrameResult(frame);
  if (result && (result->pts_ns != frame->buf_pts || result->stream_id != frame->source_id))
    result = nullptr;
  if (result) {
    if (analytics_epoch_ && *analytics_epoch_ != result->epoch) {
      smoothing_.Reset();
      // A new analytics epoch is a new fit: whatever sides were named belong
      // to the run that ended, not to this one.
      named_sides_ = false;
    }
    analytics_epoch_ = result->epoch;
  }
  size_t visible = 0;
  // Teams, when the game has named them, are the thing worth reading off a
  // minimap: two groups in one hue, dark against light, rather than 32 track
  // colours nobody can tell apart at this size. Without a side the dot is
  // hollow, which says "not known" instead of guessing a group. Ignored
  // players keep the boxes' grey override.
  const Color team_dark{0.16F, 0.18F, 0.22F, 1.0F};
  const Color team_light{0.93F, 0.94F, 0.96F, 1.0F};
  // Switch to the two-group reading only once this stream has named a side,
  // and keep reading it that way afterwards: a run with teams off keeps its
  // track colours, and a frame that happens to carry no decided track must
  // not flip the whole map back for one frame. The flag is latched from the
  // per-player walk below, so no extra pass over the results is needed.
  const size_t inspected = std::min(snapshot->player_rects.size(), player_analytics::kMaximumTracks);
  for (size_t index = 0; index < inspected; ++index) {
    const auto& player = snapshot->player_rects[index];
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
    const auto& palette = box.border_color;
    const auto channel = [](double value) {
      return std::isfinite(value) ? uint32_t(std::clamp(value, 0.0, 1.0) * 255 + .5) : 0U;
    };
    const uint32_t color_key = (channel(palette.red) << 16) | (channel(palette.green) << 8) | channel(palette.blue);
    const auto p = point(smoothing_.Update(player.track_id, frame->buf_pts, color_key, *location));
    std::string_view number;
    auto side = player_analytics::TeamLabel::kUnknown;
    if (result && player.track_id != player_analytics::kUntrackedId) {
      for (size_t i = 0; i < std::min(result->player_count, player_analytics::kMaximumTracks); ++i) {
        const auto& value = result->players[i];
        if (value.track_id != player.track_id)
          continue;
        // Same walk the jersey read already costs: affiliation rides along on
        // the entry this loop was going to find anyway.
        side = value.team.label;
        const auto& jersey = value.jersey;
        if (jersey.observed_at > frame->buf_pts || jersey.expires_at < frame->buf_pts)
          break;
        size_t length = 0;
        while (length < 2 && jersey.text[length] >= '0' && jersey.text[length] <= '9')
          ++length;
        if (length && jersey.text[length] == '\0')
          number = std::string_view(jersey.text.data(), length);
        break;
      }
    }
    // HOME and AWAY are names for the two groups; which sweater each wore is
    // what the map draws, so an unnamed game reads its labels directly and a
    // named one maps home and away back through options_.home_is_dark.
    const bool home_in_dark = options_.home_is_dark.value_or(true);
    const bool dark_side = side == player_analytics::TeamLabel::kDark ||
        (side == player_analytics::TeamLabel::kHome && home_in_dark) ||
        (side == player_analytics::TeamLabel::kAway && !home_in_dark);
    const bool light_side = side == player_analytics::TeamLabel::kLight ||
        (side == player_analytics::TeamLabel::kAway && home_in_dark) ||
        (side == player_analytics::TeamLabel::kHome && !home_in_dark);
    named_sides_ = named_sides_ || dark_side || light_side;
    const auto& box_color = player.ignored ? preview_overlay::kIgnoredPlayerColor : box.border_color;
    const Color track_color{
        float(box_color.red), float(box_color.green), float(box_color.blue), float(box_color.alpha)};
    const bool named = !player.ignored && (dark_side || light_side);
    const Color color = !named ? track_color : dark_side ? team_dark : team_light;
    const float radius = (number.empty() ? 5.0F : 10.0F) * scale;
    // A named side fills; an unknown one is left hollow over the ice so the
    // two readings cannot be mistaken for each other.
    const bool hollow = !named && !player.ignored && named_sides_;
    commands->AddDisc(p.x, p.y, radius + 1.5F * scale, white);
    if (hollow) {
      commands->AddDisc(p.x, p.y, radius, ink);
      commands->AddDisc(p.x, p.y, radius - 1.5F * scale, white);
    } else {
      commands->AddDisc(p.x, p.y, radius, color);
    }
    // Numbers read against whatever the dot ended up being filled with.
    const Color fill = hollow ? white : color;
    const Color label = fill.red * .2126F + fill.green * .7152F + fill.blue * .0722F > .5F ? ink : white;
    if (!number.empty()) {
      const float text_height = 13 * scale;
      commands->AddText(p.x - number.size() * text_height / 3, p.y - text_height / 2, text_height, number, label);
    }
    ++visible;
  }
  char summary[64];
  if (snapshot->player_rects.empty())
    std::snprintf(summary, sizeof(summary), "NO TRACKS - TEAMS UNKNOWN");
  else
    std::snprintf(
        summary,
        sizeof(summary),
        "%zu/%zu%s LOCATED - TEAMS UNKNOWN",
        visible,
        inspected,
        snapshot->player_rects.size() > inspected ? "+" : "");
  commands->AddText(left, top + header + rink_height + 4 * scale, 13 * scale, summary, ink);
  return true;
}
} // namespace hm::draw_display
