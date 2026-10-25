#include <iostream>
#include <stdexcept>
#include "hstream/src/gst-plugins/gst-rink-map/RinkMapOverlay.h"
#include "hstream/src/libs/common/ConfigLayers.h"
#include "hstream/src/libs/stitching/ConfigureStitchingCore.h"

namespace {
void Check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
} // namespace
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  try {
    auto baseline = YAML::LoadFile(argv[1]);
    const auto settings = hm::rink_map::ResolveSettings(baseline);
    Check(settings.ok() && !settings->enabled, "Baseline map default must resolve and remain opt-in");
    auto defaults = YAML::Load("output: {rink-map: {enable: true, size: large}}");
    auto game = YAML::Load("output: {rink-map: {position: lower-left}}");
    auto resolved =
        hm::config::ResolveProgramConfiguration(baseline, defaults, game, YAML::Node(YAML::NodeType::Map), {});
    Check(resolved.ok(), "Configuration layers failed");
    auto enabled = hm::rink_map::ResolveSettings(resolved->values);
    Check(
        enabled.ok() && enabled->enabled &&
            enabled->options.size == hm::draw_display::RinkMiniMapOptions::Size::kLarge &&
            enabled->options.position == hm::draw_display::RinkMiniMapOptions::Position::kLowerLeft,
        "Overlay precedence failed");
    resolved->values["output"]["rink-map"]["size"] = "bogus";
    Check(!hm::rink_map::ResolveSettings(resolved->values).ok(), "Invalid size accepted");
    Check(!hm::rink_map::ResolveSettings(YAML::Node()).ok(), "Missing defaults accepted");
    auto map = std::make_shared<hm::stitching::PlaybackRinkMap>();
    map->hugin_generation = "fixture\n";
    map->fit.state = hm::stitching::RinkMapState::kReady;
    map->fit.plane_to_ray = cv::Matx33d(0, -1, 19, -1, 0, -3, 0, 0, -7);
    map->columns = map->rows = 2;
    map->rays.assign(4, *hm::stitching::RinkPointToRay(map->fit, {0, 0}));
    hm::draw_display::RinkMapFrame frame;
    frame.generation = *hm::stitching::stitched_output_generation_id(map->hugin_generation, 0, 1600, 1200);
    frame.stitched_width = 1600;
    frame.stitched_height = 1200;
    frame.coordinate_width = 800;
    frame.coordinate_height = 600;
    frame.pts = 100;
    frame.have_tracks = true;
    frame.players.push_back({350, 100, 100, 200, {.2F, .4F, .7F, 1}, 22, false});
    hm::draw_display::RinkMiniMap renderer(map, enabled->options);
    hm::draw_display::analytics::CommandList commands;
    Check(renderer.Append(frame, 1920, 1080, &commands) && !commands.empty(), "Portable map did not draw");
    size_t discs = 0;
    for (size_t i = 0; i < commands.size(); ++i)
      discs += commands.data()[i].kind == hm::draw_display::analytics::detail::Kind::kDisc;
    Check(discs == 2, "Portable player projection lost a dot");
    GstVideoInfo info{};
    gst_video_info_set_format(&info, GST_VIDEO_FORMAT_P010_10LE, 1920, 1080);
    auto* buffer = gst_buffer_new();
    Check(hm::rink_map::AttachFrame(buffer, frame, info), "Metadata attach failed");
    auto* copy = gst_buffer_copy(buffer);
    gst_buffer_unref(buffer);
    const GstVideoInfo* copied_info = nullptr;
    auto* copied = hm::rink_map::FindFrame(copy, &copied_info);
    Check(
        copied && copied->generation == frame.generation && copied->players[0].track_id == 22 &&
            copied_info->width == 1920,
        "Metadata copy lost observations/geometry");
    gst_buffer_unref(copy);
    frame.generation = "stale";
    commands.Clear();
    Check(!renderer.Append(frame, 1920, 1080, &commands) && commands.empty(), "Stale calibration painted output");
    renderer.Reset();
    frame.generation = *hm::stitching::stitched_output_generation_id(map->hugin_generation, 0, 1600, 1200);
    frame.pts = 1;
    Check(renderer.Append(frame, 640, 360, &commands), "Reset/resize did not recover");
    std::cout << "Rink-map configuration, portable projection, metadata and reset checks passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
