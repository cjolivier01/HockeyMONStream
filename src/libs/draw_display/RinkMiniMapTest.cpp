#include "hstream/src/libs/draw_display/RinkMiniMap.h"

#include <iostream>
#include <stdexcept>

#include "hstream/src/libs/common/PreviewOverlayMeta.h"
#include "hstream/src/libs/player_analytics/FrameMeta.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

namespace {
namespace a = hm::draw_display::analytics;
namespace p = hm::preview_overlay;
namespace pa = hm::player_analytics;
namespace rink = hm::stitching;
void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Frame {
  NvDsBatchMeta* batch{nvds_create_batch_meta(1)};
  NvDsFrameMeta* frame{batch ? nvds_acquire_frame_meta_from_pool(batch) : nullptr};
  Frame(
      const std::string& generation,
      bool snapshot = true,
      bool jersey = true,
      uint64_t expires = 120,
      bool ignored = false,
      NvOSD_ColorParams color = {0.2, 0.4, 0.7, 1},
      bool overflow = false) {
    check(frame, "Metadata allocation");
    frame->source_id = 4;
    frame->buf_pts = 100;
    frame->source_frame_width = 800;
    frame->source_frame_height = 600;
    nvds_add_frame_meta_to_batch(batch, frame);
    check(rink::add_stitched_output_generation_meta(frame, generation), "Generation attachment");
    p::OverlaySnapshot observation;
    observation.coordinate_width = 800;
    observation.coordinate_height = 600;
    p::PlayerRect player;
    player.track_id = 22;
    player.rect.left = 350;
    player.rect.top = 100;
    player.rect.width = 100;
    player.rect.height = 200;
    player.rect.border_color = color;
    player.ignored = ignored;
    if (overflow) {
      auto outside = player;
      outside.rect.left = 1600;
      observation.player_rects.assign(pa::kMaximumTracks, outside);
    }
    observation.player_rects.push_back(player);
    if (snapshot)
      check(p::add_prepared_overlay_snapshot_meta(frame, observation), "Snapshot attachment");
    if (jersey) {
      pa::FrameResult result;
      result.stream_id = frame->source_id;
      result.pts_ns = frame->buf_pts;
      result.coordinate_width = 800;
      result.coordinate_height = 600;
      result.player_count = 1;
      result.players[0].track_id = 22;
      result.players[0].box = {350, 100, 100, 200};
      result.players[0].jersey.text = {'0', '7', '\0'};
      result.players[0].jersey.observed_at = 80;
      result.players[0].jersey.expires_at = expires;
      result.players[0].jersey.confidence = .9;
      check(pa::AttachFrameResult(frame, result), "Jersey attachment");
    }
  }
  ~Frame() {
    nvds_destroy_batch_meta(batch);
  }
};
size_t count(const a::CommandList& commands, a::detail::Kind kind, bool white = false) {
  size_t result = 0;
  for (size_t i = 0; i < commands.size(); ++i)
    if (commands.data()[i].kind == kind && (!white || commands.data()[i].color.red > .9F))
      ++result;
  return result;
}
bool dotColor(const a::CommandList& commands, const NvOSD_ColorParams& expected) {
  const a::detail::Command* fill = nullptr;
  for (size_t i = 0; i < commands.size(); ++i)
    if (commands.data()[i].kind == a::detail::Kind::kDisc)
      fill = &commands.data()[i];
  return fill && fill->color.red == float(expected.red) && fill->color.green == float(expected.green) &&
      fill->color.blue == float(expected.blue) && fill->color.alpha == float(expected.alpha);
}
} // namespace

int main() {
  try {
    auto map = std::make_shared<rink::PlaybackRinkMap>();
    map->hugin_generation = "fixture\n";
    map->fit.state = rink::RinkMapState::kReady;
    map->fit.plane_to_ray = cv::Matx33d(0, -1, 19, -1, 0, -3, 0, 0, -7);
    map->columns = map->rows = 2;
    map->rays.assign(4, *rink::RinkPointToRay(map->fit, {0, 0}));
    hm::draw_display::RinkMiniMap overlay(map);
    const auto generation = *rink::stitched_output_generation_id(map->hugin_generation, 0, 1600, 1200);
    a::CommandList commands;
    Frame fresh(generation);
    check(overlay.Append(fresh.frame, 1600, 1200, 800, 600, &commands), "Valid mini-map must draw");
    check(count(commands, a::detail::Kind::kDisc) == 2, "One player produces one bordered dot");
    check(dotColor(commands, {0.2, 0.4, 0.7, 1}), "Dot must use the immutable detection-box colour");
    check(
        count(commands, a::detail::Kind::kGlyph, true) == 2, "Known jersey retains both digits including leading zero");
    check(commands.rejected() == 0 && commands.size() < 200, "Mini-map stays within its drawing budget");
    commands.Clear();
    Frame ignored(generation, true, false, 120, true);
    check(overlay.Append(ignored.frame, 1600, 1200, 800, 600, &commands), "Ignored track stays visible");
    check(dotColor(commands, p::kIgnoredPlayerColor), "Ignored dots must match ignored boxes' grey colour");
    commands.Clear();
    Frame bright(generation, true, true, 120, false, {1, 1, 0, 1});
    check(overlay.Append(bright.frame, 1600, 1200, 800, 600, &commands), "Bright track stays visible");
    check(dotColor(commands, {1, 1, 0, 1}), "Bright dot retains the box colour");
    check(count(commands, a::detail::Kind::kGlyph, true) == 0, "Bright dots use dark jersey digits for contrast");
    commands.Clear();
    Frame overflow(generation, true, false, 120, false, {0.2, 0.4, 0.7, 1}, true);
    check(overlay.Append(overflow.frame, 1600, 1200, 800, 600, &commands), "Oversized snapshot retains the rink");
    check(count(commands, a::detail::Kind::kDisc) == 0, "Off-ice players must consume the bounded inspection budget");
    commands.Clear();
    Frame expired(generation, true, true, 99);
    check(overlay.Append(expired.frame, 1600, 1200, 800, 600, &commands), "Expired jersey keeps player dot");
    check(
        count(commands, a::detail::Kind::kDisc) == 2 && count(commands, a::detail::Kind::kGlyph, true) == 0,
        "Expired jersey numbers are withheld");
    commands.Clear();
    fresh.frame->buf_pts = 101;
    check(
        overlay.Append(fresh.frame, 1600, 1200, 800, 600, &commands),
        "Fresh positions survive missing current semantics");
    check(count(commands, a::detail::Kind::kGlyph, true) == 0, "Another frame's jersey metadata is ignored");
    commands.Clear();
    Frame missing(generation, false);
    check(
        !overlay.Append(missing.frame, 1600, 1200, 800, 600, &commands) && commands.empty(),
        "No snapshot must not reuse old player locations");
    Frame stale(*rink::stitched_output_generation_id("other\n", 0, 1600, 1200));
    check(
        !overlay.Append(stale.frame, 1600, 1200, 800, 600, &commands) && commands.empty(),
        "Another calibration must not reuse the old map");
    check(
        !overlay.Append(fresh.frame, 800, 600, 800, 600, &commands) && commands.empty(),
        "Mismatched live canvas dimensions are rejected");
    for (const auto display : {cv::Size(800, 600), cv::Size(600, 1200), cv::Size(2800, 360), cv::Size(160, 90)}) {
      for (bool upper : {false, true}) {
        float previous_width = 0;
        for (const char* size : {"compact", "standard", "large"}) {
          hm::draw_display::RinkMiniMap sized(
              map, hm::draw_display::RinkMiniMapPreferences(size, upper ? "upper-left" : "lower-left"));
          commands.Clear();
          check(sized.Append(fresh.frame, 1600, 1200, display.width, display.height, &commands), "Sized map draws");
          const auto& panel = commands.data()[0];
          check(
              panel.kind == a::detail::Kind::kFill && panel.x0 >= 0 && panel.y0 >= 0 && panel.x1 <= display.width &&
                  panel.y1 <= display.height,
              "Complete mini-map panel fits the image");
          check(upper ? panel.y0 < display.height / 2 : panel.y1 > display.height / 2, "Selected map corner is used");
          check(panel.x1 - panel.x0 > previous_width, "Map size presets increase the visible panel");
          previous_width = panel.x1 - panel.x0;
          check(commands.rejected() == 0, "Sized map fits the command budget");
        }
      }
    }
    std::cout << "RinkMiniMap tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
