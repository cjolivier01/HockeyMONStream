#include "hstream/src/libs/common/PreviewOverlayMeta.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace hm::preview_overlay {
namespace {

Point rotate(Point point, Point anchor, float angle_degrees) {
  constexpr float kPi = 3.14159265358979323846F;
  const float radians = angle_degrees * (kPi / 180.0F);
  const float sine = std::sin(radians);
  const float cosine = std::cos(radians);
  const float dx = point.x - anchor.x;
  const float dy = point.y - anchor.y;
  return Point{anchor.x + dx * cosine - dy * sine, anchor.y + dx * sine + dy * cosine};
}

gpointer copy_transform_meta(gpointer data, gpointer) {
  auto* user_meta = static_cast<NvDsUserMeta*>(data);
  if (!user_meta || !user_meta->user_meta_data)
    return nullptr;
  return g_memdup2(user_meta->user_meta_data, sizeof(PlayCropperTransform));
}

void release_transform_meta(gpointer data, gpointer) {
  auto* user_meta = static_cast<NvDsUserMeta*>(data);
  if (!user_meta)
    return;
  g_free(user_meta->user_meta_data);
  user_meta->user_meta_data = nullptr;
}

gpointer copy_overlay_snapshot_meta(gpointer data, gpointer) {
  auto* user_meta = static_cast<NvDsUserMeta*>(data);
  auto* snapshot = user_meta ? static_cast<const OverlaySnapshot*>(user_meta->user_meta_data) : nullptr;
  if (!snapshot)
    return nullptr;
  try {
    return new OverlaySnapshot(*snapshot);
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}

void release_overlay_snapshot_meta(gpointer data, gpointer) {
  auto* user_meta = static_cast<NvDsUserMeta*>(data);
  if (!user_meta)
    return;
  delete static_cast<OverlaySnapshot*>(user_meta->user_meta_data);
  user_meta->user_meta_data = nullptr;
}

} // namespace

Point input_to_output(const PlayCropperTransform& transform, Point point) {
  if (transform.crop_width <= 0.0F || transform.crop_height <= 0.0F)
    return {};
  Point cropped{point.x - transform.source_left, point.y - transform.source_top};
  cropped = rotate(cropped, Point{transform.anchor_x, transform.anchor_y}, transform.angle_degrees);
  return Point{
      (cropped.x - transform.crop_left) * transform.output_width / transform.crop_width,
      (cropped.y - transform.crop_top) * transform.output_height / transform.crop_height,
  };
}

Point metadata_to_output(const PlayCropperTransform& transform, Point point) {
  if (transform.metadata_width > 0.0F && transform.metadata_height > 0.0F) {
    point.x *= transform.input_width / transform.metadata_width;
    point.y *= transform.input_height / transform.metadata_height;
  }
  return input_to_output(transform, point);
}

Point output_to_input(const PlayCropperTransform& transform, Point point) {
  if (transform.output_width <= 0.0F || transform.output_height <= 0.0F)
    return {};
  Point crop_space{
      transform.crop_left + point.x * transform.crop_width / transform.output_width,
      transform.crop_top + point.y * transform.crop_height / transform.output_height,
  };
  crop_space = rotate(crop_space, Point{transform.anchor_x, transform.anchor_y}, -transform.angle_degrees);
  return Point{crop_space.x + transform.source_left, crop_space.y + transform.source_top};
}

std::vector<std::array<Point, 3>> arrow_head_triangles(
    Point start,
    Point end,
    float shaft_width,
    NvOSD_Arrow_Head_Direction direction) {
  const float dx = end.x - start.x;
  const float dy = end.y - start.y;
  const float length = std::hypot(dx, dy);
  if (length <= 0.001F)
    return {};
  const Point unit{dx / length, dy / length};
  const Point normal{-unit.y, unit.x};
  const float head_length = std::min(length * 0.5F, std::max(8.0F, 4.0F * shaft_width));
  const float head_half_width = std::min(length * 0.25F, std::max(4.0F, 2.0F * shaft_width));
  std::vector<std::array<Point, 3>> triangles;
  triangles.reserve(direction == BOTH_HEAD ? 2U : 1U);
  auto add_head = [&](Point tip, Point toward_shaft) {
    const Point base{tip.x + toward_shaft.x * head_length, tip.y + toward_shaft.y * head_length};
    triangles.push_back({
        tip,
        Point{base.x + normal.x * head_half_width, base.y + normal.y * head_half_width},
        Point{base.x - normal.x * head_half_width, base.y - normal.y * head_half_width},
    });
  };
  if (direction == START_HEAD || direction == BOTH_HEAD)
    add_head(start, unit);
  if (direction == END_HEAD || direction == BOTH_HEAD)
    add_head(end, Point{-unit.x, -unit.y});
  return triangles;
}

NvDsMetaType playcropper_transform_meta_type() {
  static const NvDsMetaType type = nvds_get_user_meta_type(const_cast<gchar*>("HSTREAM.PLAYCROPPER_TRANSFORM"));
  return type;
}

bool add_playcropper_transform_meta(NvDsFrameMeta* frame_meta, const PlayCropperTransform& transform) {
  if (!frame_meta || !frame_meta->base_meta.batch_meta)
    return false;
  if (find_playcropper_transform_meta(frame_meta))
    return false;
  gpointer payload = g_memdup2(&transform, sizeof(transform));
  if (!payload)
    return false;
  NvDsUserMeta* user_meta = nvds_acquire_user_meta_from_pool(frame_meta->base_meta.batch_meta);
  if (!user_meta) {
    g_free(payload);
    return false;
  }
  user_meta->user_meta_data = payload;
  user_meta->base_meta.meta_type = playcropper_transform_meta_type();
  user_meta->base_meta.copy_func = copy_transform_meta;
  user_meta->base_meta.release_func = release_transform_meta;
  nvds_add_user_meta_to_frame(frame_meta, user_meta);
  return true;
}

const PlayCropperTransform* find_playcropper_transform_meta(const NvDsFrameMeta* frame_meta) {
  if (!frame_meta)
    return nullptr;
  for (NvDsMetaList* item = frame_meta->frame_user_meta_list; item; item = item->next) {
    auto* user_meta = static_cast<NvDsUserMeta*>(item->data);
    if (user_meta && user_meta->base_meta.meta_type == playcropper_transform_meta_type())
      return static_cast<const PlayCropperTransform*>(user_meta->user_meta_data);
  }
  return nullptr;
}

NvDsMetaType overlay_snapshot_meta_type() {
  static const NvDsMetaType type = nvds_get_user_meta_type(const_cast<gchar*>("HSTREAM.PREVIEW_OVERLAY_SNAPSHOT"));
  return type;
}

bool add_overlay_snapshot_meta(NvDsFrameMeta* frame_meta) {
  if (!frame_meta || !frame_meta->base_meta.batch_meta)
    return false;
  if (find_overlay_snapshot_meta(frame_meta))
    return true;
  try {
    auto snapshot = std::make_unique<OverlaySnapshot>();
    snapshot->coordinate_width = static_cast<float>(frame_meta->source_frame_width);
    snapshot->coordinate_height = static_cast<float>(frame_meta->source_frame_height);
    snapshot->player_rects.reserve(frame_meta->num_obj_meta);
    for (NvDsMetaList* item = frame_meta->obj_meta_list; item; item = item->next) {
      auto* object_meta = static_cast<NvDsObjectMeta*>(item->data);
      if (object_meta && object_meta->class_id == 0 && object_meta->object_id != UNTRACKED_OBJECT_ID)
        snapshot->player_rects.push_back(object_meta->rect_params);
    }
    for (NvDsMetaList* item = frame_meta->display_meta_list; item; item = item->next) {
      auto* display_meta = static_cast<NvDsDisplayMeta*>(item->data);
      if (!display_meta)
        continue;
      snapshot->play_rects.insert(
          snapshot->play_rects.end(), display_meta->rect_params, display_meta->rect_params + display_meta->num_rects);
      snapshot->play_lines.insert(
          snapshot->play_lines.end(), display_meta->line_params, display_meta->line_params + display_meta->num_lines);
      snapshot->play_arrows.insert(
          snapshot->play_arrows.end(),
          display_meta->arrow_params,
          display_meta->arrow_params + display_meta->num_arrows);
      snapshot->play_circles.insert(
          snapshot->play_circles.end(),
          display_meta->circle_params,
          display_meta->circle_params + display_meta->num_circles);
    }
    NvDsUserMeta* user_meta = nvds_acquire_user_meta_from_pool(frame_meta->base_meta.batch_meta);
    if (!user_meta)
      return false;
    user_meta->user_meta_data = snapshot.release();
    user_meta->base_meta.meta_type = overlay_snapshot_meta_type();
    user_meta->base_meta.copy_func = copy_overlay_snapshot_meta;
    user_meta->base_meta.release_func = release_overlay_snapshot_meta;
    nvds_add_user_meta_to_frame(frame_meta, user_meta);
    return true;
  } catch (const std::bad_alloc&) {
    return false;
  }
}

const OverlaySnapshot* find_overlay_snapshot_meta(const NvDsFrameMeta* frame_meta) {
  if (!frame_meta)
    return nullptr;
  for (NvDsMetaList* item = frame_meta->frame_user_meta_list; item; item = item->next) {
    auto* user_meta = static_cast<NvDsUserMeta*>(item->data);
    if (user_meta && user_meta->base_meta.meta_type == overlay_snapshot_meta_type())
      return static_cast<const OverlaySnapshot*>(user_meta->user_meta_data);
  }
  return nullptr;
}

} // namespace hm::preview_overlay
