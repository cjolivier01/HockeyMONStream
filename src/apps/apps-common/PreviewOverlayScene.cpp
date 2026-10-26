#include "PreviewOverlayScene.h"
#include "hstream/src/libs/common/PersonRoles.h"

#include "hstream/src/libs/draw_display/PlayerOverlays.h"
#include "hstream/src/libs/draw_display/RinkMiniMap.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hm::gpu_preview {
namespace {

OverlayColor overlay_color(const NvOSD_ColorParams& color) {
  return OverlayColor{
      static_cast<float>(color.red),
      static_cast<float>(color.green),
      static_cast<float>(color.blue),
      static_cast<float>(color.alpha),
  };
}

std::vector<hm::preview_overlay::Point> transform_points(
    const std::vector<hm::preview_overlay::Point>& points,
    const hm::preview_overlay::PlayCropperTransform* transform) {
  if (!transform)
    return points;
  std::vector<hm::preview_overlay::Point> transformed;
  transformed.reserve(points.size());
  for (const auto& point : points)
    transformed.push_back(hm::preview_overlay::metadata_to_output(*transform, point));
  return transformed;
}

void add_rect_paths(
    PreviewOverlayScene* overlays,
    float left,
    float top,
    float width,
    float height,
    float border_width,
    OverlayColor border,
    bool has_background,
    OverlayColor background,
    const hm::preview_overlay::PlayCropperTransform* transform) {
  if (!overlays || width <= 0.0F || height <= 0.0F)
    return;
  const std::vector<hm::preview_overlay::Point> raw = {
      {left, top},
      {left + width, top},
      {left + width, top + height},
      {left, top + height},
  };
  const auto points = transform_points(raw, transform);
  if (has_background && background.alpha > 0.0F)
    overlays->paths.push_back(OverlayPath{points, background, 1.0F, true, true});
  if (border_width > 0.0F && border.alpha > 0.0F)
    overlays->paths.push_back(OverlayPath{points, border, border_width, true, false});
}

} // namespace

PreviewOverlayScene BuildPreviewOverlayScene(
    const NvDsFrameMeta* frame_meta,
    const PreviewOverlayOptions& options,
    draw_display::analytics::CommandList& commands,
    draw_display::RinkMiniMap* rink_mini_map) {
  PreviewOverlayScene overlays;
  commands.Clear();
  overlays.coordinate_width = static_cast<float>(options.negotiated_width);
  overlays.coordinate_height = static_cast<float>(options.negotiated_height);
  overlays.stitched_surface_width =
      static_cast<float>(options.source_width ? options.source_width : options.negotiated_width);
  overlays.stitched_surface_height =
      static_cast<float>(options.source_height ? options.source_height : options.negotiated_height);
  if (!frame_meta) {
    overlays.rink_map_unavailable_reason = "The preview frame is missing its frame metadata.";
    return overlays;
  }
  if (const auto* generation = hm::stitching::find_stitched_output_generation_meta(frame_meta)) {
    overlays.stitched_output_generation = generation->generation();
  }
  const auto* snapshot = hm::preview_overlay::find_overlay_snapshot_meta(frame_meta);
  const auto* attached_transform =
      options.channel == "program" ? hm::preview_overlay::find_playcropper_transform_meta(frame_meta) : nullptr;
  if (options.channel == "program" && !attached_transform) {
    // Snapshot geometry is captured before playcropper, while this sink sees
    // the cropped/rotated Program surface. If the bounded DeepStream user-meta
    // pool could not provide a transform slot, drawing either the snapshot or
    // the stitched rink mask in that input coordinate space is misleading.
    // Object/display metadata also has no proof of its coordinate space
    // without the transform's object_meta_transformed marker, so fail closed
    // for every diagnostic layer on this frame while continuing to show video.
    overlays.coordinate_width = static_cast<float>(options.negotiated_width);
    overlays.coordinate_height = static_cast<float>(options.negotiated_height);
    overlays.diagnostic_coordinates_valid = false;
    overlays.rink_map_unavailable_reason = "The preview frame is missing its Program crop geometry.";
    return overlays;
  }
  if (options.channel == "program" && attached_transform) {
    overlays.program_transform = *attached_transform;
    overlays.coordinate_width = attached_transform->output_width;
    overlays.coordinate_height = attached_transform->output_height;
    overlays.stitched_surface_width = attached_transform->input_width;
    overlays.stitched_surface_height = attached_transform->input_height;
  } else if (snapshot) {
    overlays.coordinate_width = snapshot->coordinate_width;
    overlays.coordinate_height = snapshot->coordinate_height;
  } else {
    overlays.coordinate_width = static_cast<float>(frame_meta->source_frame_width);
    overlays.coordinate_height = static_cast<float>(frame_meta->source_frame_height);
  }
  if (overlays.coordinate_width <= 0.0F || overlays.coordinate_height <= 0.0F) {
    overlays.coordinate_width = static_cast<float>(options.negotiated_width);
    overlays.coordinate_height = static_cast<float>(options.negotiated_height);
  }
  const auto* program_transform = overlays.program_transform ? &*overlays.program_transform : nullptr;

  if (options.show_player_tracking &&
      (!program_transform || !(program_transform->baked_player_layers & hm::player_analytics::kDrawPlayerBoxes))) {
    auto add_player_rect = [&](const NvOSD_RectParams& rect,
                               bool ignored,
                               const hm::preview_overlay::PlayCropperTransform* transform,
                               std::string_view role) {
      add_rect_paths(
          &overlays,
          rect.left,
          rect.top,
          rect.width,
          rect.height,
          std::max(2.0F, static_cast<float>(rect.border_width)),
          overlay_color(ignored ? hm::preview_overlay::kIgnoredPlayerColor : rect.border_color),
          false,
          {},
          transform);
      if (!role.empty()) {
        const auto point = transform ? hm::preview_overlay::metadata_to_output(*transform, {rect.left, rect.top})
                                     : hm::preview_overlay::Point{rect.left, rect.top};
        const float height = 24 * std::clamp(overlays.coordinate_height / 1080.0F, 0.5F, 8.0F);
        commands.AddText(point.x, std::max(0.0F, point.y - height), height, role, {1, 1, 1, 1});
      }
      if (ignored && role.empty())
        hm::draw_display::analytics::AddIgnoredPlayerLabel(
            rect, transform, overlays.coordinate_width, overlays.coordinate_height, &commands);
    };
    if (snapshot) {
      for (const auto& player : snapshot->player_rects)
        add_player_rect(player.rect, player.ignored, program_transform, player.role_label);
    } else {
      const auto* object_transform =
          program_transform && !program_transform->object_meta_transformed ? program_transform : nullptr;
      for (NvDsMetaList* item = frame_meta->obj_meta_list; item; item = item->next) {
        auto* object_meta = static_cast<NvDsObjectMeta*>(item->data);
        if (!object_meta || !hm::IsPerson(object_meta->class_id, object_meta->obj_label) ||
            object_meta->object_id == UNTRACKED_OBJECT_ID)
          continue;
        add_player_rect(
            object_meta->rect_params,
            hm::preview_overlay::player_is_ignored(*object_meta),
            object_transform,
            hm::PersonRoleText(object_meta->obj_label));
      }
    }
  }

  if (options.show_play_tracking) {
    auto add_play_rect = [&](const NvOSD_RectParams& rect) {
      add_rect_paths(
          &overlays,
          rect.left,
          rect.top,
          rect.width,
          rect.height,
          static_cast<float>(rect.border_width),
          overlay_color(rect.border_color),
          rect.has_bg_color,
          overlay_color(rect.bg_color),
          program_transform);
    };
    auto add_play_line = [&](const NvOSD_LineParams& line) {
      overlays.paths.push_back(
          OverlayPath{
              transform_points(
                  {{static_cast<float>(line.x1), static_cast<float>(line.y1)},
                   {static_cast<float>(line.x2), static_cast<float>(line.y2)}},
                  program_transform),
              overlay_color(line.line_color),
              static_cast<float>(line.line_width),
              false,
              false,
          });
    };
    auto add_play_arrow = [&](const NvOSD_ArrowParams& arrow) {
      const OverlayColor color = overlay_color(arrow.arrow_color);
      const float width = static_cast<float>(arrow.arrow_width);
      overlays.paths.push_back(
          OverlayPath{
              transform_points(
                  {{static_cast<float>(arrow.x1), static_cast<float>(arrow.y1)},
                   {static_cast<float>(arrow.x2), static_cast<float>(arrow.y2)}},
                  program_transform),
              color,
              width,
              false,
              false,
          });
      for (const auto& triangle : hm::preview_overlay::arrow_head_triangles(
               {static_cast<float>(arrow.x1), static_cast<float>(arrow.y1)},
               {static_cast<float>(arrow.x2), static_cast<float>(arrow.y2)},
               width,
               arrow.arrow_head)) {
        overlays.paths.push_back(
            OverlayPath{
                transform_points(
                    std::vector<hm::preview_overlay::Point>(triangle.begin(), triangle.end()), program_transform),
                color,
                1.0F,
                true,
                true});
      }
    };
    auto add_play_circle = [&](const NvOSD_CircleParams& circle) {
      constexpr int kCircleSegments = 48;
      std::vector<hm::preview_overlay::Point> points;
      points.reserve(kCircleSegments);
      for (int segment = 0; segment < kCircleSegments; ++segment) {
        constexpr float kPi = 3.14159265358979323846F;
        const float angle = 2.0F * kPi * segment / kCircleSegments;
        points.push_back({
            static_cast<float>(circle.xc) + static_cast<float>(circle.radius) * std::cos(angle),
            static_cast<float>(circle.yc) + static_cast<float>(circle.radius) * std::sin(angle),
        });
      }
      points = transform_points(points, program_transform);
      if (circle.has_bg_color)
        overlays.paths.push_back(OverlayPath{points, overlay_color(circle.bg_color), 1.0F, true, true});
      overlays.paths.push_back(OverlayPath{points, overlay_color(circle.circle_color), 3.0F, true, false});
    };
    if (snapshot) {
      for (const NvOSD_RectParams& rect : snapshot->play_rects)
        add_play_rect(rect);
      for (const NvOSD_LineParams& line : snapshot->play_lines)
        add_play_line(line);
      for (const NvOSD_ArrowParams& arrow : snapshot->play_arrows)
        add_play_arrow(arrow);
      for (const NvOSD_CircleParams& circle : snapshot->play_circles)
        add_play_circle(circle);
    } else {
      for (NvDsMetaList* item = frame_meta->display_meta_list; item; item = item->next) {
        auto* display_meta = static_cast<NvDsDisplayMeta*>(item->data);
        if (!display_meta)
          continue;
        for (guint index = 0; index < display_meta->num_rects; ++index)
          add_play_rect(display_meta->rect_params[index]);
        for (guint index = 0; index < display_meta->num_lines; ++index)
          add_play_line(display_meta->line_params[index]);
        for (guint index = 0; index < display_meta->num_arrows; ++index)
          add_play_arrow(display_meta->arrow_params[index]);
        for (guint index = 0; index < display_meta->num_circles; ++index)
          add_play_circle(display_meta->circle_params[index]);
      }
    }
  }
  if (options.player_analytics_layers)
    hm::draw_display::analytics::BuildPlayerOverlays(
        frame_meta,
        options.player_analytics_layers,
        options.player_joint_confidence,
        program_transform,
        overlays.coordinate_width,
        overlays.coordinate_height,
        &commands);
  if (rink_mini_map && (options.channel == "program" || options.channel == "stitched")) {
    const auto width = ExactPreviewDimension(overlays.stitched_surface_width);
    const auto height = ExactPreviewDimension(overlays.stitched_surface_height);
    const char* reason = "The preview frame is missing its stitched canvas dimensions.";
    if (width && height) {
      rink_mini_map->Append(
          frame_meta, *width, *height, overlays.coordinate_width, overlays.coordinate_height, &commands);
      reason = rink_mini_map->unavailableReason();
    }
    overlays.rink_map_unavailable_reason = reason;
  }
  return overlays;
}

} // namespace hm::gpu_preview
