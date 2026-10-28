#include "hstream/src/apps/apps-common/PreviewOverlayScene.h"

#include "hstream/src/libs/draw_display/RinkMiniMap.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

#include <gst/gst.h>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace preview = hm::preview_overlay;
namespace scene = hm::gpu_preview;
namespace analytics = hm::draw_display::analytics;

void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void Point(const preview::Point& point, float x, float y) {
  Check(std::abs(point.x - x) < 0.001F && std::abs(point.y - y) < 0.001F, "incorrect overlay coordinates");
}

struct Frame {
  NvDsBatchMeta* batch = nvds_create_batch_meta(1);
  NvDsFrameMeta* frame = nullptr;
  Frame() {
    Check(batch, "batch allocation");
    frame = nvds_acquire_frame_meta_from_pool(batch);
    Check(frame, "frame allocation");
    frame->source_frame_width = 800;
    frame->source_frame_height = 600;
    nvds_add_frame_meta_to_batch(batch, frame);
  }
  ~Frame() {
    nvds_destroy_batch_meta(batch);
  }
};

void GeometryAndFrozenMetadata() {
  Frame fixture;
  auto* object = nvds_acquire_obj_meta_from_pool(fixture.batch);
  Check(object, "object allocation");
  object->class_id = 0;
  object->object_id = 17;
  object->rect_params.left = 200;
  object->rect_params.top = 100;
  object->rect_params.width = 80;
  object->rect_params.height = 120;
  object->rect_params.border_width = 4;
  object->rect_params.border_color = {0.2, 0.4, 0.8, 1};
  nvds_add_obj_meta_to_frame(fixture.frame, object, nullptr);
  auto* display = nvds_acquire_display_meta_from_pool(fixture.batch);
  Check(display, "display allocation");
  display->num_lines = 1;
  display->line_params[0] = {200, 100, 280, 220, 5, {1, 0, 0, 0.5}};
  nvds_add_display_meta_to_frame(fixture.frame, display);
  Check(preview::add_overlay_snapshot_meta(fixture.frame), "snapshot attachment");
  Check(hm::stitching::add_stitched_output_generation_meta(fixture.frame, "saved-generation"), "generation attachment");
  // Frozen original metadata must win over downstream object/display changes.
  object->rect_params.left = 999;
  display->line_params[0].x1 = 999;
  preview::PlayCropperTransform transform{400, 300, 800, 600, 20, 10, 0, 0, 30, 20, 200, 100, 400, 200, 0, true, 0};
  Check(preview::add_playcropper_transform_meta(fixture.frame, transform), "transform attachment");
  scene::PreviewOverlayOptions options{"program", 100, 50, 400, 300, true, true, 0, 0.3F};
  analytics::CommandList commands;
  const auto program = scene::BuildPreviewOverlayScene(fixture.frame, options, commands, nullptr);
  Check(program.diagnostic_coordinates_valid && program.program_transform.has_value(), "Program transform lost");
  Check(program.coordinate_width == 400 && program.coordinate_height == 200, "Program output geometry lost");
  Check(program.stitched_surface_width == 400 && program.stitched_surface_height == 300, "input surface geometry lost");
  Check(program.stitched_output_generation == "saved-generation", "calibration identity lost");
  Check(program.paths.size() == 2 && commands.empty(), "unexpected Program paths");
  Point(program.paths[0].points[0], 100, 40);
  Point(program.paths[0].points[2], 180, 160);
  Point(program.paths[1].points[0], 100, 40);
  Point(program.paths[1].points[1], 180, 160);
  Check(program.paths[0].closed && !program.paths[0].filled && program.paths[0].width == 4, "box style changed");
  Check(program.paths[1].color.alpha == 0.5F && program.paths[1].width == 5, "line style changed");

  options.channel = "stitched";
  const auto stitched = scene::BuildPreviewOverlayScene(fixture.frame, options, commands, nullptr);
  Check(
      !stitched.program_transform && stitched.coordinate_width == 800 && stitched.coordinate_height == 600,
      "Stitched reused Program geometry");
  Check(
      stitched.stitched_surface_width == 400 && stitched.stitched_surface_height == 300,
      "metadata dimensions replaced the actual stitched surface");
  Point(stitched.paths[0].points[0], 200, 100);
  Point(stitched.paths[1].points[0], 200, 100);

  options.show_player_tracking = options.show_play_tracking = false;
  Check(commands.AddDisc(10, 10, 4, {1, 1, 1, 1}), "stale command fixture");
  const auto hidden = scene::BuildPreviewOverlayScene(fixture.frame, options, commands, nullptr);
  Check(hidden.paths.empty() && commands.empty(), "disabled overlay collection retained earlier commands");
  const auto absent = scene::BuildPreviewOverlayScene(nullptr, options, commands, nullptr);
  Check(
      absent.paths.empty() && commands.empty() && absent.stitched_output_generation.empty() &&
          absent.rink_map_unavailable_reason,
      "missing frame retained earlier metadata or readiness");
  // Previously returned scenes own their paths, independent of later calls.
  Point(program.paths[0].points[0], 100, 40);
}

void MissingAndAlreadyTransformedGeometry() {
  Frame fixture;
  auto* object = nvds_acquire_obj_meta_from_pool(fixture.batch);
  Check(object, "object allocation");
  object->class_id = 0;
  object->object_id = 9;
  object->rect_params.left = 50;
  object->rect_params.top = 20;
  object->rect_params.width = 40;
  object->rect_params.height = 60;
  object->rect_params.border_color = {1, 1, 1, 1};
  nvds_add_obj_meta_to_frame(fixture.frame, object, nullptr);
  scene::PreviewOverlayOptions options{"program", 400, 200, 400, 300, true, false, 0, 0.3F};
  analytics::CommandList commands;
  Check(commands.AddDisc(10, 10, 4, {1, 1, 1, 1}), "stale command fixture");
  const auto missing = scene::BuildPreviewOverlayScene(fixture.frame, options, commands, nullptr);
  Check(
      !missing.diagnostic_coordinates_valid && missing.paths.empty() && commands.empty() &&
          missing.rink_map_unavailable_reason,
      "missing Program transform did not fail closed");
  const preview::PlayCropperTransform transform{
      400, 300, 800, 600, 20, 10, 0, 0, 30, 20, 200, 100, 400, 200, 0, true, 0};
  Check(preview::add_playcropper_transform_meta(fixture.frame, transform), "transform attachment");
  const auto already_transformed = scene::BuildPreviewOverlayScene(fixture.frame, options, commands, nullptr);
  Check(already_transformed.paths.size() == 1, "fallback object metadata omitted");
  Point(already_transformed.paths[0].points[0], 50, 20);
  Point(already_transformed.paths[0].points[2], 90, 80);
  options.channel = "source0";
  options.source_width = options.source_height = 0;
  const auto source = scene::BuildPreviewOverlayScene(fixture.frame, options, commands, nullptr);
  Check(
      !source.program_transform && source.coordinate_width == 800 && source.coordinate_height == 600 &&
          source.stitched_surface_width == 400 && source.stitched_surface_height == 200,
      "source fallback geometry changed");
}

void ExactArtifactGeometry() {
  Check(scene::ExactPreviewDimension(16384.0F) == 16384, "exact canvas rejected");
  for (float dimension :
       {0.0F,
        -1.0F,
        12.5F,
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN(),
        4294967296.0F}) {
    Check(!scene::ExactPreviewDimension(dimension), "invalid canvas dimension accepted");
  }
}

void MiniMapRouteAndFrameGeometry() {
  auto map = std::make_shared<hm::stitching::PlaybackRinkMap>();
  map->hugin_generation = "preview-map-fixture\n";
  map->fit.state = hm::stitching::RinkMapState::kReady;
  map->fit.plane_to_ray = cv::Matx33d(0, -1, 19, -1, 0, -3, 0, 0, -7);
  map->columns = map->rows = 2;
  map->rays.assign(4, *hm::stitching::RinkPointToRay(map->fit, {0, 0}));
  const auto generation = *hm::stitching::stitched_output_generation_id(map->hugin_generation, 0, 800, 600);
  Frame fixture;
  Check(preview::add_overlay_snapshot_meta(fixture.frame), "mini-map snapshot attachment");
  Check(hm::stitching::add_stitched_output_generation_meta(fixture.frame, generation), "mini-map identity attachment");
  const preview::PlayCropperTransform transform{800, 600, 800, 600, 0, 0, 0, 0, 0, 0, 800, 600, 400, 300, 0, true, 0};
  Check(preview::add_playcropper_transform_meta(fixture.frame, transform), "mini-map crop attachment");
  scene::PreviewOverlayOptions options{"program", 400, 300, 800, 600, false, false, 0, 0.3F};
  analytics::CommandList commands;
  hm::draw_display::RinkMiniMap program(map), stitched(map);
  for (const char* channel : {"program", "stitched"}) {
    options.channel = channel;
    auto* renderer = options.channel == "program" ? &program : &stitched;
    const auto painted = scene::BuildPreviewOverlayScene(fixture.frame, options, commands, renderer);
    Check(
        !painted.rink_map_unavailable_reason && commands.size() > 80 && painted.paths.empty(),
        "Program/Stitched mini-map must draw while every other drawing layer is off");
  }
  options.channel = "source0";
  scene::BuildPreviewOverlayScene(fixture.frame, options, commands, &program);
  Check(commands.empty(), "Source route inherited mini-map commands");
  options.channel = "stitched";
  options.source_width = 801;
  const auto resized = scene::BuildPreviewOverlayScene(fixture.frame, options, commands, &stitched);
  Check(resized.rink_map_unavailable_reason && commands.empty(), "Different canvas accepted a saved mini-map");
  options.source_width = 800;
  Frame missing;
  Check(hm::stitching::add_stitched_output_generation_meta(missing.frame, generation), "missing crop identity");
  options.channel = "program";
  const auto crop_missing = scene::BuildPreviewOverlayScene(missing.frame, options, commands, &program);
  Check(
      crop_missing.rink_map_unavailable_reason && commands.empty(),
      "Mini-map retained readiness without matching Program crop metadata");
  options.channel = "stitched";
  Frame stale;
  Check(hm::stitching::add_stitched_output_generation_meta(stale.frame, "another-generation"), "stale map identity");
  const auto mismatch = scene::BuildPreviewOverlayScene(stale.frame, options, commands, &stitched);
  Check(mismatch.rink_map_unavailable_reason && commands.empty(), "Stale calibration retained mini-map commands");
  const auto absent = scene::BuildPreviewOverlayScene(nullptr, options, commands, &stitched);
  Check(absent.rink_map_unavailable_reason && commands.empty(), "Absent frame retained mini-map readiness");
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  try {
    GeometryAndFrozenMetadata();
    MissingAndAlreadyTransformedGeometry();
    ExactArtifactGeometry();
    MiniMapRouteAndFrameGeometry();
    std::cout << "Shared preview overlay geometry, frozen metadata, frame clearing and artifact bounds passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
