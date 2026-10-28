#include "hstream/src/libs/draw_display/RinkMiniMap.h"

#include <iostream>
#include <stdexcept>

#include "hstream/src/libs/common/PreviewOverlayMeta.h"
#include "hstream/src/libs/draw_display/AnalyticsOverlayInternal.h"
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
      bool overflow = false,
      double shift_x = 0,
      uint64_t pts_ns = 100,
      pa::TeamLabel team = pa::TeamLabel::kUnknown,
      // 0 none, 1 after the decided track, 2 before it. The order matters:
      // the overlay has to read the same way whichever one it walks first.
      int second_undecided_track = 0,
      // A snapshot that reached the overlay with nobody in it, which is what a
      // whistle or a fully pruned frame looks like.
      bool empty_snapshot = false,
      // Put the decided track where the saved map draws no dot for it.
      bool decided_off_board = false) {
    check(frame, "Metadata allocation");
    frame->source_id = 4;
    frame->buf_pts = pts_ns;
    frame->source_frame_width = 800;
    frame->source_frame_height = 600;
    nvds_add_frame_meta_to_batch(batch, frame);
    check(rink::add_stitched_output_generation_meta(frame, generation), "Generation attachment");
    p::OverlaySnapshot observation;
    observation.coordinate_width = 800;
    observation.coordinate_height = 600;
    p::PlayerRect player;
    player.track_id = 22;
    player.rect.left = decided_off_board ? 1600 : 350 + shift_x;
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
    // A skater the clusterer has not called yet, on the ice beside one it has.
    // Both are tracked and both are drawn.
    auto undecided = player;
    undecided.track_id = 23;
    undecided.rect.left = 300 + shift_x;
    undecided.ignored = false;
    if (second_undecided_track == 2)
      observation.player_rects.push_back(undecided);
    if (!empty_snapshot)
      observation.player_rects.push_back(player);
    if (second_undecided_track == 1)
      observation.player_rects.push_back(undecided);
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
      if (team != pa::TeamLabel::kUnknown) {
        // A named side carries the evidence that named it; the metadata
        // validator refuses a label without its cluster and observation.
        result.players[0].team.label = team;
        result.players[0].team.cluster = team == pa::TeamLabel::kHome || team == pa::TeamLabel::kDark ? 0 : 1;
        result.players[0].team.evidence = 2.0F;
        result.players[0].team.observed_at = pts_ns;
      }
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
// The footer is the last text the overlay adds, and it is the only text on its
// baseline, so the trailing glyphs sharing the last command's y are its own
// rather than a jersey number's. AddText drops spaces, so the reading has none.
// A misread changes the string the comparisons below see, so it fails them; it
// cannot turn a wrong footer into a passing one.
std::string footer(const a::CommandList& commands) {
  size_t end = commands.size();
  while (end && commands.data()[end - 1].kind != a::detail::Kind::kGlyph)
    --end;
  if (!end)
    return {};
  const float baseline = commands.data()[end - 1].y0;
  size_t begin = end;
  while (begin && commands.data()[begin - 1].kind == a::detail::Kind::kGlyph &&
         commands.data()[begin - 1].y0 == baseline)
    --begin;
  std::string text;
  for (size_t i = begin; i < end; ++i) {
    // AddText stores the atlas index, which carries the weight offset.
    uint32_t code = commands.data()[i].glyph;
    if (code >= uint32_t(a::detail::kWeightGlyphOffset))
      code -= uint32_t(a::detail::kWeightGlyphOffset);
    text.push_back(char(code + a::detail::kFirstGlyph));
  }
  return text;
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
    const auto portable = hm::draw_display::ReadRinkMapFrame(fresh.frame, 1600, 1200);
    check(portable.analytics.get() == pa::FindFrameResult(fresh.frame), "Adapter must share immutable analytics");
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
    {
      // A named side uses the home/away palette instead of its track colour.
      // Each case gets its own overlay because the two-group reading latches
      // per stream, which is what keeps it steady between frames.
      hm::draw_display::RinkMiniMap named(map);
      Frame home(generation, true, true, 120, false, {0.2, 0.4, 0.7, 1}, false, 0, 100, pa::TeamLabel::kHome);
      check(named.Append(home.frame, 1600, 1200, 800, 600, &commands), "The home side stays visible");
      check(dotColor(commands, {0, 0, 128.0 / 255, 1}), "Home fills navy");
      check(count(commands, a::detail::Kind::kGlyph, true) == 2, "Navy dots use light jersey digits");
      check(footer(commands) == "1/1LOCATED", "A footer over named dots cannot keep reporting the teams as unknown");
      commands.Clear();
    }
    {
      // The camera policy excludes the nearest skater on most frames of a real
      // game, and that track is often the first the clusterer calls. It is
      // drawn grey, not in a side's colour, so on its own it does not make the
      // map a two-group reading and the footer must keep saying so.
      hm::draw_display::RinkMiniMap excluded(map);
      Frame ignored_side(generation, true, true, 120, true, {0.2, 0.4, 0.7, 1}, false, 0, 100, pa::TeamLabel::kHome);
      check(excluded.Append(ignored_side.frame, 1600, 1200, 800, 600, &commands), "An excluded named track draws");
      check(dotColor(commands, p::kIgnoredPlayerColor), "An excluded track stays grey even with a side");
      check(
          footer(commands) == "1/1LOCATED-TEAMSUNKNOWN",
          "An excluded track's side cannot announce a naming no dot on the map shows");
      commands.Clear();
    }
    {
      // A whistle empties the snapshot for a few frames. The sides this stream
      // named do not become unknown again while nobody is on screen.
      hm::draw_display::RinkMiniMap settled(map);
      Frame named(generation, true, true, 120, false, {0.2, 0.4, 0.7, 1}, false, 0, 100, pa::TeamLabel::kHome);
      check(settled.Append(named.frame, 1600, 1200, 800, 600, &commands), "The naming frame draws");
      commands.Clear();
      Frame empty(
          generation, true, false, 120, false, {0.2, 0.4, 0.7, 1}, false, 0, 140, pa::TeamLabel::kUnknown, false, true);
      check(settled.Append(empty.frame, 1600, 1200, 800, 600, &commands), "An empty snapshot retains the rink");
      check(footer(commands) == "NOTRACKS", "An empty frame cannot withdraw a naming this stream already made");
      commands.Clear();
      hm::draw_display::RinkMiniMap fresh_overlay(map);
      Frame empty_unnamed(
          generation, true, false, 120, false, {0.2, 0.4, 0.7, 1}, false, 0, 140, pa::TeamLabel::kUnknown, false, true);
      check(fresh_overlay.Append(empty_unnamed.frame, 1600, 1200, 800, 600, &commands), "An empty first frame draws");
      check(
          footer(commands) == "NOTRACKS-TEAMSUNKNOWN",
          "A run that has never named a side still reports the teams as unknown");
      commands.Clear();
    }
    {
      hm::draw_display::RinkMiniMap named(map);
      Frame light_side(generation, true, true, 120, false, {0.2, 0.4, 0.7, 1}, false, 0, 100, pa::TeamLabel::kLight);
      check(named.Append(light_side.frame, 1600, 1200, 800, 600, &commands), "A named light side stays visible");
      check(dotColor(commands, {0.93, 0.94, 0.96, 1}), "The light side fills with the light team colour");
      check(
          count(commands, a::detail::Kind::kGlyph, true) == 0,
          "Digits on a light dot are drawn dark so they stay readable");
      commands.Clear();
    }
    {
      // Someone on the ice has a side, so a skater without one is drawn
      // hollow rather than filled with a colour that reads as a third group.
      // Which of the two the overlay walks first cannot change that, so both
      // orders are checked: the reading is decided before anything is drawn.
      for (const int order : {1, 2}) {
        hm::draw_display::RinkMiniMap named(map);
        Frame unknown_side(
            generation, true, true, 120, false, {0.2, 0.4, 0.7, 1}, false, 0, 100, pa::TeamLabel::kDark, order);
        check(named.Append(unknown_side.frame, 1600, 1200, 800, 600, &commands), "An unnamed skater stays visible");
        check(
            count(commands, a::detail::Kind::kDisc) == 5,
            "A named skater is a filled dot and an undecided one beside them is a ring");
        commands.Clear();
      }
      hm::draw_display::RinkMiniMap named(map);
      Frame unknown_side(
          generation, true, true, 120, false, {0.2, 0.4, 0.7, 1}, false, 0, 100, pa::TeamLabel::kDark, 1);
      check(named.Append(unknown_side.frame, 1600, 1200, 800, 600, &commands), "An unnamed skater stays visible");
      check(dotColor(commands, {0.96, 0.98, 1, 1}), "The ring is left open over the ice");
      commands.Clear();
    }
    {
      // The saved map draws no dot for a player outside the estimated boards.
      // A side called on one of those cannot switch the map into the two-group
      // reading either, for the same reason an excluded track cannot.
      hm::draw_display::RinkMiniMap off_board(map);
      Frame outside_named(
          generation, true, true, 120, false, {0.2, 0.4, 0.7, 1}, false, 0, 100, pa::TeamLabel::kHome, 2, false, true);
      check(off_board.Append(outside_named.frame, 1600, 1200, 800, 600, &commands), "The skater on the ice draws");
      check(count(commands, a::detail::Kind::kDisc) == 2, "Only the located skater gets a dot");
      check(dotColor(commands, {0.2, 0.4, 0.7, 1}), "That skater keeps its track colour instead of a ring");
      check(
          footer(commands) == "1/2LOCATED-TEAMSUNKNOWN",
          "A side on a track the map cannot place does not make the map a two-group reading");
      commands.Clear();
    }
    {
      // Sweater choice cannot reverse the home/away colours.
      for (const bool home_is_dark : {false, true}) {
        for (const auto side :
             {pa::TeamLabel::kHome, pa::TeamLabel::kAway, pa::TeamLabel::kDark, pa::TeamLabel::kLight}) {
          hm::draw_display::RinkMiniMap named(map, {{}, {}, home_is_dark});
          Frame player(generation, true, true, 120, false, {0.2, 0.4, 0.7, 1}, false, 0, 100, side);
          check(named.Append(player.frame, 1600, 1200, 800, 600, &commands), "A named side draws");
          const bool home = side == pa::TeamLabel::kHome || (side == pa::TeamLabel::kDark && home_is_dark) ||
              (side == pa::TeamLabel::kLight && !home_is_dark);
          check(
              dotColor(
                  commands, home ? NvOSD_ColorParams{0, 0, 128.0 / 255, 1} : NvOSD_ColorParams{1, 140.0 / 255, 0, 1}),
              "Home stays navy and away stays orange for either sweater choice");
          check(
              count(commands, a::detail::Kind::kGlyph, true) == (home ? 2 : 0),
              "Jersey digits contrast with the navy/orange fill");
          commands.Clear();
        }
      }
    }
    {
      // Once a side is named the reading stays: a later frame carrying no
      // decided track is an unknown skater, not a game without teams.
      hm::draw_display::RinkMiniMap settled(map);
      Frame named(generation, true, true, 120, false, {0.2, 0.4, 0.7, 1}, false, 0, 100, pa::TeamLabel::kDark);
      check(settled.Append(named.frame, 1600, 1200, 800, 600, &commands), "The naming frame draws");
      commands.Clear();
      Frame undecided(generation, true, true, 120, false, {0.2, 0.4, 0.7, 1}, false, 0, 140);
      check(settled.Append(undecided.frame, 1600, 1200, 800, 600, &commands), "The following frame draws");
      check(
          count(commands, a::detail::Kind::kDisc) == 3,
          "A frame with no decided track keeps the two-group reading instead of flipping back");
      commands.Clear();
    }
    {
      // A second stream's naming is not this one's. Switching sources clears
      // the reading rather than drawing one game's teams over another's.
      hm::draw_display::RinkMiniMap shared(map);
      Frame named(generation, true, true, 120, false, {0.2, 0.4, 0.7, 1}, false, 0, 100, pa::TeamLabel::kDark);
      check(shared.Append(named.frame, 1600, 1200, 800, 600, &commands), "the naming frame draws");
      commands.Clear();
      Frame other(generation, true, true, 120, false, {0.2, 0.4, 0.7, 1}, false, 0, 140);
      other.frame->source_id = 9;
      check(shared.Append(other.frame, 1600, 1200, 800, 600, &commands), "another source draws");
      check(
          count(commands, a::detail::Kind::kDisc) == 2 && dotColor(commands, {0.2, 0.4, 0.7, 1}),
          "A different source starts from its own track colours, not the last one's teams");
      commands.Clear();
    }
    // Teams off: nothing has been named on this stream, so the track colours
    // stay as they were rather than every dot going hollow. A fresh overlay,
    // because the two-group reading latches per stream once a side appears.
    hm::draw_display::RinkMiniMap teamless(map);
    Frame unnamed(generation, true, true, 120, false, {0.2, 0.4, 0.7, 1});
    check(teamless.Append(unnamed.frame, 1600, 1200, 800, 600, &commands), "An unnamed run still draws");
    check(dotColor(commands, {0.2, 0.4, 0.7, 1}), "Without any named side the dots keep their track colour");
    check(count(commands, a::detail::Kind::kDisc) == 2, "An unnamed dot is a plain filled dot");
    check(footer(commands) == "1/1LOCATED-TEAMSUNKNOWN", "Track-coloured dots still say the teams are unknown");
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
        overlay.Append(missing.frame, 1600, 1200, 800, 600, &commands) && count(commands, a::detail::Kind::kDisc) == 0,
        "A missing track snapshot retains the rink outline without reusing old player positions");
    commands.Clear();
    Frame stale(*rink::stitched_output_generation_id("other\n", 0, 1600, 1200));
    check(
        !overlay.Append(stale.frame, 1600, 1200, 800, 600, &commands) && commands.empty(),
        "Another calibration must not reuse the old map");
    check(
        overlay.unavailableReason() &&
            std::string(overlay.unavailableReason()).find("calibration") != std::string::npos,
        "Hidden maps explain their incompatible frame identity");
    check(
        !overlay.Append(fresh.frame, 800, 600, 800, 600, &commands) && commands.empty(),
        "Mismatched live canvas dimensions are rejected");
    for (const auto display : {cv::Size(800, 600), cv::Size(600, 1200), cv::Size(2800, 360), cv::Size(160, 90)}) {
      for (const char* position : {"upper-right", "upper-left", "lower-left", ""}) {
        const bool upper = std::string_view(position) != "lower-left";
        const bool right = std::string_view(position) == "upper-right" || std::string_view(position).empty();
        float previous_width = 0, previous_height = 0;
        for (const char* size : {"icon", "compact", "standard", "large"}) {
          hm::draw_display::RinkMiniMap sized(map, hm::draw_display::RinkMiniMapPreferences(size, position));
          commands.Clear();
          check(sized.Append(fresh.frame, 1600, 1200, display.width, display.height, &commands), "Sized map draws");
          const auto& panel = commands.data()[0];
          check(
              panel.kind == a::detail::Kind::kFill && panel.x0 >= 0 && panel.y0 >= 0 && panel.x1 <= display.width &&
                  panel.y1 <= display.height,
              "Complete mini-map panel fits the image");
          check(upper ? panel.y0 < display.height / 2 : panel.y1 > display.height / 2, "Selected map corner is used");
          check(right ? panel.x1 > display.width / 2 : panel.x0 < display.width / 2, "Selected map side is used");
          check(panel.x1 - panel.x0 > previous_width, "Map size presets increase the visible panel");
          if (std::string_view(size) == "compact") {
            check(std::abs((panel.x1 - panel.x0) / previous_width - 2) < .001, "Icon is half the width of Compact");
            check(std::abs((panel.y1 - panel.y0) / previous_height - 2) < .001, "Icon is half the height of Compact");
          }
          previous_width = panel.x1 - panel.x0;
          previous_height = panel.y1 - panel.y0;
          check(commands.rejected() == 0, "Sized map fits the command budget");
        }
      }
    }
    auto moving_map = std::make_shared<rink::PlaybackRinkMap>(*map);
    moving_map->rays = {
        *rink::RinkPointToRay(map->fit, {-10, 5}),
        *rink::RinkPointToRay(map->fit, {10, 5}),
        *rink::RinkPointToRay(map->fit, {-10, -5}),
        *rink::RinkPointToRay(map->fit, {10, -5})};
    hm::draw_display::RinkMiniMap stable(moving_map);
    const auto draw_dot = [&](hm::draw_display::RinkMiniMap& renderer, Frame& input) {
      commands.Clear();
      check(renderer.Append(input.frame, 1600, 1200, 800, 600, &commands), "Smoothing frame renders");
      cv::Point2d point(-1, -1);
      for (size_t i = 0; i < commands.size(); ++i)
        if (commands.data()[i].kind == a::detail::Kind::kDisc)
          point = {commands.data()[i].x0, commands.data()[i].y0};
      return point;
    };
    Frame first(generation, true, false, 0, false, {.2, .4, .7, 1}, false, 0, 1000000000);
    const auto origin = draw_dot(stable, first);
    Frame jitter(generation, true, false, 0, false, {.2, .4, .7, 1}, false, 50, 1033333333);
    hm::draw_display::RinkMiniMap raw(moving_map);
    const auto measured = draw_dot(raw, jitter), damped = draw_dot(stable, jitter);
    check(cv::norm(damped - origin) < cv::norm(measured - origin) * .7, "Displayed dots damp box-foot movement");
    Frame seek(generation, true, false, 0, false, {.2, .4, .7, 1}, false, 50, 500000000);
    check(draw_dot(stable, seek) == measured, "Backward seeks reset displayed history");
    Frame ahead(generation, true, false, 0, false, {.2, .4, .7, 1}, false, 0, 20000000000);
    check(cv::norm(draw_dot(stable, ahead) - origin) < 1e-5, "Forward seeks reset displayed history");
    Frame lost(generation, false, false);
    commands.Clear();
    check(
        stable.Append(lost.frame, 1600, 1200, 800, 600, &commands) && count(commands, a::detail::Kind::kDisc) == 0,
        "Missing metadata withholds all dots while retaining the rink");
    jitter.frame->buf_pts = 20033333333;
    check(draw_dot(stable, jitter) == measured, "Missing metadata clears old smoothing state");
    std::cout << "RinkMiniMap tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
