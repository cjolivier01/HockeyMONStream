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
  Frame(const std::string& generation, bool snapshot = true, bool jersey = true, uint64_t expires = 120) {
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
    check(
        count(commands, a::detail::Kind::kGlyph, true) == 2, "Known jersey retains both digits including leading zero");
    check(commands.rejected() == 0 && commands.size() < 200, "Mini-map stays within its drawing budget");
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
    std::cout << "RinkMiniMap tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
