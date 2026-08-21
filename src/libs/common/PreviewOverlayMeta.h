#pragma once

#include <nvdsmeta.h>

#include <vector>

namespace hm::preview_overlay {

struct Point {
  float x{0.0F};
  float y{0.0F};
};

// Describes the affine stitched-canvas -> Program transform used by
// playcropper for one frame. Preview-only consumers use it to align debug
// metadata and the saved rink mask without touching Program video pixels.
struct PlayCropperTransform {
  float input_width{0.0F};
  float input_height{0.0F};
  float metadata_width{0.0F};
  float metadata_height{0.0F};
  float source_left{0.0F};
  float source_top{0.0F};
  float anchor_x{0.0F};
  float anchor_y{0.0F};
  float crop_left{0.0F};
  float crop_top{0.0F};
  float crop_width{0.0F};
  float crop_height{0.0F};
  float output_width{0.0F};
  float output_height{0.0F};
  float angle_degrees{0.0F};
  bool object_meta_transformed{false};
};

// Immutable pre-playcropper metadata copied at the tracked Stitched tee. Both
// preview branches consume this snapshot so downstream object-meta transforms
// cannot race the Stitched renderer through a shared GstBuffer.
struct OverlaySnapshot {
  float coordinate_width{0.0F};
  float coordinate_height{0.0F};
  std::vector<NvOSD_RectParams> player_rects;
  std::vector<NvOSD_RectParams> play_rects;
  std::vector<NvOSD_LineParams> play_lines;
  std::vector<NvOSD_ArrowParams> play_arrows;
  std::vector<NvOSD_CircleParams> play_circles;
};

Point input_to_output(const PlayCropperTransform& transform, Point point);
Point metadata_to_output(const PlayCropperTransform& transform, Point point);
Point output_to_input(const PlayCropperTransform& transform, Point point);

NvDsMetaType playcropper_transform_meta_type();
bool add_playcropper_transform_meta(NvDsFrameMeta* frame_meta, const PlayCropperTransform& transform);
const PlayCropperTransform* find_playcropper_transform_meta(const NvDsFrameMeta* frame_meta);

NvDsMetaType overlay_snapshot_meta_type();
bool add_overlay_snapshot_meta(NvDsFrameMeta* frame_meta);
const OverlaySnapshot* find_overlay_snapshot_meta(const NvDsFrameMeta* frame_meta);

} // namespace hm::preview_overlay
