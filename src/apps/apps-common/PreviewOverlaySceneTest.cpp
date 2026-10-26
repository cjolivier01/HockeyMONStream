#include "hstream/src/apps/apps-common/PreviewOverlayScene.h"

#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"
#include "hstream/src/libs/player_analytics/FrameMeta.h"

#include <gst/gst.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

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
  void SetTransform(const preview::PlayCropperTransform& transform) {
    for (auto* item = frame->frame_user_meta_list; item; item = item->next) {
      auto* meta = static_cast<NvDsUserMeta*>(item->data);
      if (meta->base_meta.meta_type == preview::playcropper_transform_meta_type()) {
        nvds_remove_user_meta_from_frame(frame, meta);
        break;
      }
    }
    Check(preview::add_playcropper_transform_meta(frame, transform), "attach crop transform");
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

void RoleAndSemanticLabels() {
  namespace pa = hm::player_analytics;
  constexpr unsigned semantic_layers = pa::kDrawJerseys | pa::kDrawActions | pa::kDrawTeams;
  for (const char* label : {"", "person", "goalie", "official"}) {
    const std::string role = std::strcmp(label, "goalie") == 0 ? "GOALIE"
        : std::strcmp(label, "official") == 0                  ? "REF"
                                                               : "";
    for (bool frozen : {false, true}) {
      for (bool program : {false, true}) {
        Frame fixture;
        auto* object = nvds_acquire_obj_meta_from_pool(fixture.batch);
        Check(object, "role object allocation");
        object->class_id = role.empty() ? 0 : role == "GOALIE" ? 1 : 2;
        object->object_id = 17;
        std::strcpy(object->obj_label, label);
        object->rect_params.left = 200;
        object->rect_params.top = 100;
        object->rect_params.width = 80;
        object->rect_params.height = 120;
        object->rect_params.border_color = {1, 1, 1, 1};
        nvds_add_obj_meta_to_frame(fixture.frame, object, nullptr);
        pa::FrameResult result;
        result.stream_id = fixture.frame->source_id;
        result.pts_ns = fixture.frame->buf_pts;
        result.coordinate_width = 800;
        result.coordinate_height = 600;
        // Only goalies and ordinary players receive squad analytics.
        result.player_count = role == "REF" ? 0 : 1;
        auto& player = result.players[0];
        player.track_id = 17;
        player.box = {200, 100, 80, 120};
        player.jersey.text = {'0', '7', '\0'};
        player.jersey.observed_at = result.pts_ns;
        player.jersey.expires_at = result.pts_ns;
        player.action.label = 0;
        player.action.window_start = player.action.observed_at = result.pts_ns;
        player.action.expires_at = result.pts_ns;
        std::strcpy(player.action_text.data(), "falling");
        player.team.label = pa::TeamLabel::kHome;
        player.team.cluster = 0;
        player.team.observed_at = result.pts_ns;
        Check(pa::AttachFrameResult(fixture.frame, result), "attach role analytics");
        if (frozen) {
          Check(preview::add_overlay_snapshot_meta(fixture.frame), "attach role snapshot");
          // The preview and semantic row reservation must use frozen roles.
          object->class_id = 0;
          std::strcpy(object->obj_label, "person");
          object->rect_params.left = 999;
        }
        preview::PlayCropperTransform transform{};
        transform.input_width = transform.metadata_width = transform.crop_width = transform.output_width = 800;
        transform.input_height = transform.metadata_height = transform.crop_height = transform.output_height = 600;
        transform.anchor_x = 400;
        transform.anchor_y = 300;
        transform.angle_degrees = 30;
        if (program)
          Check(preview::add_playcropper_transform_meta(fixture.frame, transform), "attach role crop transform");
        scene::PreviewOverlayOptions options{
            program ? "program" : "stitched", 800, 600, 800, 600, true, false, semantic_layers, .3F};
        analytics::CommandList commands;
        const auto combined = scene::BuildPreviewOverlayScene(fixture.frame, options, commands, nullptr);
        const size_t semantic_glyphs = result.player_count ? 13 : 0;
        Check(combined.paths.size() == 1, "preview role box missing");
        Check(commands.size() == role.size() + semantic_glyphs, "preview added or lost label glyphs");
        std::string text;
        for (size_t i = 0; i < commands.size(); ++i) {
          Check(commands.data()[i].kind == analytics::detail::Kind::kGlyph, "unexpected label command");
          text += static_cast<char>(commands.data()[i].glyph + 32);
        }
        Check(text == role + (result.player_count ? "07fallingHOME" : ""), "unexpected role/player text");
        float semantic_y = 0;
        if (semantic_glyphs) {
          semantic_y = commands.data()[role.size()].y0;
          if (!role.empty())
            Check(semantic_y + .001F >= commands.data()[0].y1, "preview role overlaps semantic text");
          Check(
              commands.data()[role.size() + 2].y0 + .001F >= commands.data()[role.size()].y1 &&
                  commands.data()[role.size() + 9].y0 + .001F >= commands.data()[role.size() + 2].y1,
              "preview semantic rows overlap");
        }
        options.show_player_tracking = false;
        const auto hidden = scene::BuildPreviewOverlayScene(fixture.frame, options, commands, nullptr);
        Check(hidden.paths.empty() && commands.size() == semantic_glyphs, "hidden preview boxes emitted role text");
        if (semantic_glyphs)
          Check(std::abs(commands.data()[0].y0 - semantic_y) < .001F, "preview toggle moved semantic rows");
        options.show_player_tracking = true;
        options.player_analytics_layers = 0;
        scene::BuildPreviewOverlayScene(fixture.frame, options, commands, nullptr);
        Check(commands.size() == role.size(), "box-only preview added person/player text");
        if (program) {
          transform.baked_player_layers = pa::kDrawPlayerBoxes;
          fixture.SetTransform(transform);
          options.player_analytics_layers = semantic_layers;
          const auto baked = scene::BuildPreviewOverlayScene(fixture.frame, options, commands, nullptr);
          Check(baked.paths.empty() && commands.size() == semantic_glyphs, "preview redrew baked role labels");
          if (semantic_glyphs)
            Check(std::abs(commands.data()[0].y0 - semantic_y) < .001F, "baked preview role row was not reserved");
          transform.baked_player_layers |= semantic_layers;
          fixture.SetTransform(transform);
          scene::BuildPreviewOverlayScene(fixture.frame, options, commands, nullptr);
          Check(commands.empty(), "preview redrew fully baked labels");
        }
      }
    }
  }
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  try {
    GeometryAndFrozenMetadata();
    MissingAndAlreadyTransformedGeometry();
    ExactArtifactGeometry();
    RoleAndSemanticLabels();
    std::cout << "Shared preview overlay geometry, frozen metadata, frame clearing and artifact bounds passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
