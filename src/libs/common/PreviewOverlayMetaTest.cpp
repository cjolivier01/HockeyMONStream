#include "hstream/src/libs/common/PreviewOverlayMeta.h"

#include <gstnvdsmeta.h>

#include <cmath>
#include <iostream>

namespace {

bool near(float actual, float expected) {
  return std::abs(actual - expected) < 0.001F;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  const hm::preview_overlay::PlayCropperTransform transform{
      4000.0F,
      2000.0F,
      2000.0F,
      1000.0F,
      500.0F,
      100.0F,
      1500.0F,
      900.0F,
      300.0F,
      200.0F,
      2400.0F,
      1350.0F,
      1920.0F,
      1080.0F,
      7.5F,
      false,
  };
  for (const hm::preview_overlay::Point point : {
           hm::preview_overlay::Point{500.0F, 100.0F},
           hm::preview_overlay::Point{2000.0F, 1000.0F},
           hm::preview_overlay::Point{3500.0F, 1900.0F},
       }) {
    const auto output = hm::preview_overlay::input_to_output(transform, point);
    const auto recovered = hm::preview_overlay::output_to_input(transform, output);
    if (!near(recovered.x, point.x) || !near(recovered.y, point.y)) {
      std::cerr << "Playcropper preview transform did not round-trip: " << recovered.x << ',' << recovered.y << '\n';
      return 1;
    }
  }
  const auto scaled_metadata = hm::preview_overlay::metadata_to_output(transform, {1000.0F, 500.0F});
  const auto scaled_input = hm::preview_overlay::input_to_output(transform, {2000.0F, 1000.0F});
  if (!near(scaled_metadata.x, scaled_input.x) || !near(scaled_metadata.y, scaled_input.y)) {
    std::cerr << "Playcropper preview transform did not scale metadata into input-surface coordinates\n";
    return 1;
  }

  const auto end_head = hm::preview_overlay::arrow_head_triangles({0.0F, 0.0F}, {100.0F, 0.0F}, 2.0F, END_HEAD);
  const auto start_head = hm::preview_overlay::arrow_head_triangles({0.0F, 0.0F}, {100.0F, 0.0F}, 2.0F, START_HEAD);
  const auto both_heads = hm::preview_overlay::arrow_head_triangles({0.0F, 0.0F}, {100.0F, 0.0F}, 2.0F, BOTH_HEAD);
  if (end_head.size() != 1 || !near(end_head[0][0].x, 100.0F) || !near(end_head[0][0].y, 0.0F) ||
      end_head[0][1].x >= end_head[0][0].x || start_head.size() != 1 || !near(start_head[0][0].x, 0.0F) ||
      !near(start_head[0][0].y, 0.0F) || start_head[0][1].x <= start_head[0][0].x || both_heads.size() != 2 ||
      !near(both_heads[0][0].x, 0.0F) || !near(both_heads[1][0].x, 100.0F) ||
      !hm::preview_overlay::arrow_head_triangles({1.0F, 1.0F}, {1.0F, 1.0F}, 2.0F, BOTH_HEAD).empty()) {
    std::cerr << "GPU preview arrow-head geometry did not preserve DeepStream direction semantics\n";
    return 2;
  }

  NvDsBatchMeta* batch_meta = nvds_create_batch_meta(1);
  NvDsFrameMeta* frame_meta = batch_meta ? nvds_acquire_frame_meta_from_pool(batch_meta) : nullptr;
  NvDsObjectMeta* player_meta = batch_meta ? nvds_acquire_obj_meta_from_pool(batch_meta) : nullptr;
  NvDsObjectMeta* nonplayer_meta = batch_meta ? nvds_acquire_obj_meta_from_pool(batch_meta) : nullptr;
  NvDsDisplayMeta* display_meta = batch_meta ? nvds_acquire_display_meta_from_pool(batch_meta) : nullptr;
  if (!batch_meta || !frame_meta || !player_meta || !nonplayer_meta || !display_meta) {
    std::cerr << "Could not allocate DeepStream metadata for preview snapshot test\n";
    if (batch_meta)
      nvds_destroy_batch_meta(batch_meta);
    return 3;
  }
  frame_meta->source_frame_width = 4096;
  frame_meta->source_frame_height = 2048;
  nvds_add_frame_meta_to_batch(batch_meta, frame_meta);
  // nvds_create_batch_meta() allocates the requested frame pool but leaves
  // max_frames_in_batch unset in this DeepStream build. Production streammux
  // metadata sets it, and the GstMeta copy callback requires it.
  batch_meta->max_frames_in_batch = 1;

  player_meta->class_id = 0;
  player_meta->object_id = 17;
  player_meta->rect_params.left = 123.0F;
  player_meta->rect_params.top = 234.0F;
  player_meta->rect_params.width = 56.0F;
  player_meta->rect_params.height = 78.0F;
  player_meta->rect_params.border_width = 4;
  nvds_add_obj_meta_to_frame(frame_meta, player_meta, nullptr);
  nonplayer_meta->class_id = 1;
  nonplayer_meta->object_id = 18;
  nonplayer_meta->rect_params.left = 900.0F;
  nvds_add_obj_meta_to_frame(frame_meta, nonplayer_meta, nullptr);

  display_meta->num_rects = 1;
  display_meta->rect_params[0].left = 345.0F;
  display_meta->rect_params[0].top = 456.0F;
  display_meta->rect_params[0].width = 100.0F;
  display_meta->rect_params[0].height = 200.0F;
  display_meta->num_lines = 1;
  display_meta->line_params[0].x1 = 10;
  display_meta->line_params[0].y1 = 20;
  display_meta->line_params[0].x2 = 30;
  display_meta->line_params[0].y2 = 40;
  display_meta->num_arrows = 1;
  display_meta->arrow_params[0].x1 = 50;
  display_meta->arrow_params[0].y1 = 60;
  display_meta->arrow_params[0].x2 = 70;
  display_meta->arrow_params[0].y2 = 80;
  display_meta->num_circles = 1;
  display_meta->circle_params[0].xc = 91;
  display_meta->circle_params[0].yc = 92;
  display_meta->circle_params[0].radius = 9;
  nvds_add_display_meta_to_frame(frame_meta, display_meta);

  if (!hm::preview_overlay::add_overlay_snapshot_meta(frame_meta)) {
    std::cerr << "Could not attach immutable preview overlay snapshot\n";
    nvds_destroy_batch_meta(batch_meta);
    return 4;
  }
  const auto* snapshot = hm::preview_overlay::find_overlay_snapshot_meta(frame_meta);
  player_meta->rect_params.left = 999.0F;
  display_meta->rect_params[0].left = 998.0F;
  display_meta->line_params[0].x1 = 997;
  display_meta->arrow_params[0].x1 = 996;
  display_meta->circle_params[0].xc = 995;
  const bool snapshot_ok = snapshot && near(snapshot->coordinate_width, 4096.0F) &&
      near(snapshot->coordinate_height, 2048.0F) && snapshot->player_rects.size() == 1 &&
      near(snapshot->player_rects[0].left, 123.0F) && snapshot->play_rects.size() == 1 &&
      near(snapshot->play_rects[0].left, 345.0F) && snapshot->play_lines.size() == 1 &&
      snapshot->play_lines[0].x1 == 10 && snapshot->play_arrows.size() == 1 && snapshot->play_arrows[0].x1 == 50 &&
      snapshot->play_circles.size() == 1 && snapshot->play_circles[0].xc == 91;
  if (!snapshot_ok || !hm::preview_overlay::add_overlay_snapshot_meta(frame_meta) ||
      hm::preview_overlay::find_overlay_snapshot_meta(frame_meta) != snapshot) {
    std::cerr << "Preview overlay snapshot changed with downstream metadata or was attached twice\n";
    nvds_destroy_batch_meta(batch_meta);
    return 5;
  }

  GstBuffer* tee_input = gst_buffer_new();
  NvDsMeta* gst_meta = tee_input
      ? gst_buffer_add_nvds_meta(
            tee_input, batch_meta, nullptr, nvds_batch_meta_copy_func, nvds_batch_meta_release_func)
      : nullptr;
  if (!gst_meta) {
    std::cerr << "Could not attach DeepStream batch metadata to the tee input\n";
    if (tee_input)
      gst_buffer_unref(tee_input);
    nvds_destroy_batch_meta(batch_meta);
    return 6;
  }
  gst_meta->meta_type = NVDS_BATCH_GST_META;
  GstBuffer* program_output = gst_buffer_new();
  GstBuffer* stitched_output = gst_buffer_new();
  const bool copied = program_output && stitched_output &&
      gst_buffer_copy_into(program_output, tee_input, GST_BUFFER_COPY_META, 0, -1) &&
      gst_buffer_copy_into(stitched_output, tee_input, GST_BUFFER_COPY_META, 0, -1);
  NvDsBatchMeta* program_batch = copied ? gst_buffer_get_nvds_batch_meta(program_output) : nullptr;
  NvDsBatchMeta* stitched_batch = copied ? gst_buffer_get_nvds_batch_meta(stitched_output) : nullptr;
  auto* program_frame = program_batch && program_batch->frame_meta_list
      ? static_cast<NvDsFrameMeta*>(program_batch->frame_meta_list->data)
      : nullptr;
  auto* stitched_frame = stitched_batch && stitched_batch->frame_meta_list
      ? static_cast<NvDsFrameMeta*>(stitched_batch->frame_meta_list->data)
      : nullptr;
  const auto* program_snapshot = hm::preview_overlay::find_overlay_snapshot_meta(program_frame);
  const auto* stitched_snapshot = hm::preview_overlay::find_overlay_snapshot_meta(stitched_frame);
  const bool attached = hm::preview_overlay::add_playcropper_transform_meta(program_frame, transform);
  const auto* attached_transform = hm::preview_overlay::find_playcropper_transform_meta(program_frame);
  auto duplicate_transform = transform;
  duplicate_transform.angle_degrees = 13.0F;
  const bool duplicate_rejected =
      !hm::preview_overlay::add_playcropper_transform_meta(program_frame, duplicate_transform);
  const bool isolated = copied && program_frame && stitched_frame && program_snapshot && stitched_snapshot &&
      program_snapshot != stitched_snapshot && attached && attached_transform &&
      near(attached_transform->angle_degrees, transform.angle_degrees) && duplicate_rejected &&
      !hm::preview_overlay::find_playcropper_transform_meta(stitched_frame) &&
      program_snapshot->player_rects.size() == 1 && stitched_snapshot->player_rects.size() == 1 &&
      near(program_snapshot->player_rects[0].left, 123.0F) && near(stitched_snapshot->player_rects[0].left, 123.0F);
  if (program_output)
    gst_buffer_unref(program_output);
  if (stitched_output)
    gst_buffer_unref(stitched_output);
  gst_buffer_unref(tee_input);
  if (!isolated) {
    std::cerr << "Program transform metadata was not isolated from copied Stitched snapshot metadata\n";
    return 7;
  }
  return 0;
}
