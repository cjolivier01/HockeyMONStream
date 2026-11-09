#include "hstream/src/apps/apps-common/TiledInference.h"
#include "gstnvdsmeta.h"
#include "nvbufsurface.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
} // namespace
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  try {
    NvBufSurfaceParams pixels{};
    pixels.width = 1000;
    pixels.height = 400;
    NvBufSurface surface{};
    surface.surfaceList = &pixels;
    surface.numFilled = surface.batchSize = 1;
    auto* buffer = gst_buffer_new_wrapped_full(
        GST_MEMORY_FLAG_READONLY, &surface, sizeof(surface), 0, sizeof(surface), nullptr, nullptr);
    auto* batch = nvds_create_batch_meta(1);
    batch->max_frames_in_batch = 1;
    auto* frame = nvds_acquire_frame_meta_from_pool(batch);
    frame->frame_num = 42;
    frame->source_id = 7;
    frame->buf_pts = GST_SECOND;
    nvds_add_frame_meta_to_batch(batch, frame);
    auto* meta =
        gst_buffer_add_nvds_meta(buffer, batch, nullptr, nvds_batch_meta_copy_func, nvds_batch_meta_release_func);
    meta->meta_type = NVDS_BATCH_GST_META;
    const hm::detection::TileInferenceSettings settings{.2, .5, 2};
    Check(hm::pipeline::PrepareTiledInferenceBuffer(buffer, settings).ok(), "Tile preparation failed");
    Check(frame->num_obj_meta == 2 && batch->num_frames_in_batch == 1, "Tiles changed frame identity");
    std::vector<NvDsObjectMeta*> tiles;
    for (auto* item = frame->obj_meta_list; item; item = item->next)
      tiles.push_back(static_cast<NvDsObjectMeta*>(item->data));
    std::sort(tiles.begin(), tiles.end(), [](const auto* a, const auto* b) { return a->class_id < b->class_id; });
    auto add = [&](NvDsObjectMeta* parent, float left, float confidence, int role) {
      auto* object = nvds_acquire_obj_meta_from_pool(batch);
      object->unique_component_id = 1;
      object->class_id = role;
      object->confidence = confidence;
      object->object_id = UNTRACKED_OBJECT_ID;
      object->rect_params.left = left;
      object->rect_params.top = 100;
      object->rect_params.width = 30;
      object->rect_params.height = 100;
      nvds_add_obj_meta_to_frame(frame, object, parent);
      return object;
    };
    add(tiles[0], 480, .7, 0);
    add(tiles[1], 481, .9, 0);
    add(tiles[0], 480, .8, 2); // Distinct role must survive class-aware NMS.
    add(tiles[0], 600, .9, 0); // Padding/outside tile is discarded.
    add(nullptr, 10, .4, 1); // Unrelated upstream metadata is preserved.
    Check(hm::pipeline::MergeTiledInferenceBuffer(buffer, settings).ok(), "Tile merge failed");
    Check(
        frame->num_obj_meta == 3 && frame->frame_num == 42 && frame->source_id == 7 && frame->buf_pts == GST_SECOND &&
            frame->bInferDone,
        "Merge changed identity or retained duplicates");
    for (auto* item = frame->obj_meta_list; item; item = item->next) {
      auto* object = static_cast<NvDsObjectMeta*>(item->data);
      Check(!object->parent && object->unique_component_id == 1, "Temporary ROI leaked downstream");
      if (object->class_id == 0)
        Check(
            object->confidence == .9f && object->detector_bbox_info.org_bbox_coords.left == 481,
            "Wrong seam detection/bbox retained");
    }
    // Empty inference results still clear both regions and mark completion.
    Check(hm::pipeline::PrepareTiledInferenceBuffer(buffer, settings).ok(), "Second frame preparation");
    Check(
        hm::pipeline::MergeTiledInferenceBuffer(buffer, settings).ok() && frame->num_obj_meta == 3,
        "Empty inference leaked regions");
    // Match the production sink probe's header-only copy at a shared tee.
    auto* sibling = gst_buffer_ref(buffer);
    auto* private_buffer = gst_buffer_make_writable(buffer);
    Check(private_buffer != sibling, "Shared buffer was not isolated");
    Check(hm::pipeline::PrepareTiledInferenceBuffer(private_buffer, settings).ok(), "Private tile preparation");
    Check(frame->num_obj_meta == 3, "Tile regions leaked into the sibling branch");
    Check(hm::pipeline::MergeTiledInferenceBuffer(private_buffer, settings).ok(), "Private tile merge");
    gst_buffer_unref(private_buffer);
    buffer = sibling;
    batch->num_frames_in_batch = 2;
    Check(!hm::pipeline::PrepareTiledInferenceBuffer(buffer, settings).ok(), "Multiple source frames accepted");
    batch->num_frames_in_batch = 1;
    // Reuse the frame with a four-quadrant plan and synthetic panorama-space
    // detections at both seams, the four-way overlap, and the lower-right edge.
    while (frame->obj_meta_list)
      nvds_remove_obj_meta_from_frame(frame, static_cast<NvDsObjectMeta*>(frame->obj_meta_list->data));
    pixels.height = 800;
    const hm::detection::TileInferenceSettings quadrants{.2, .5, 4};
    Check(hm::pipeline::PrepareTiledInferenceBuffer(buffer, quadrants).ok(), "Quadrant preparation failed");
    Check(frame->num_obj_meta == 4, "Missing quadrants");
    tiles.clear();
    for (auto* item = frame->obj_meta_list; item; item = item->next)
      tiles.push_back(static_cast<NvDsObjectMeta*>(item->data));
    std::sort(tiles.begin(), tiles.end(), [](const auto* a, const auto* b) { return a->class_id < b->class_id; });
    auto quadrant_box = [&](unsigned tile, float left, float top, float confidence, int role) {
      auto* object = add(tiles[tile], left, confidence, role);
      object->rect_params.top = top;
      object->rect_params.height = 30;
    };
    for (unsigned index = 0; index < 4; ++index)
      quadrant_box(index, 480, 380, .6f + index * .1f, 0);
    quadrant_box(0, 480, 380, .85, 2); // A different role survives.
    quadrant_box(0, 100, 380, .7, 0);
    quadrant_box(2, 101, 381, .8, 0); // Horizontal seam.
    quadrant_box(0, 480, 100, .7, 0);
    quadrant_box(1, 481, 101, .8, 0); // Vertical seam.
    quadrant_box(3, 900, 700, .5, 0);
    quadrant_box(3, 990, 790, .4, 0); // Clip at both panorama edges.
    quadrant_box(3, 100, 100, .9, 0); // Entirely outside this quadrant.
    Check(hm::pipeline::MergeTiledInferenceBuffer(buffer, quadrants).ok(), "Quadrant merge failed");
    Check(
        frame->num_obj_meta == 6 && frame->frame_num == 42 && frame->buf_pts == GST_SECOND && frame->source_id == 7 &&
            frame->bInferDone,
        "Quadrant merge lost identity or detections");
    bool bottom_right = false, clipped = false;
    for (auto* item = frame->obj_meta_list; item; item = item->next) {
      auto* object = static_cast<NvDsObjectMeta*>(item->data);
      const auto& rect = object->rect_params;
      Check(!object->parent && object->unique_component_id == 1, "Quadrant ROI escaped downstream");
      bottom_right |= rect.left == 900 && rect.top == 700;
      clipped |= rect.left == 990 && rect.top == 790 && rect.width == 10 && rect.height == 10;
      Check(object->detector_bbox_info.org_bbox_coords.top == rect.top, "Detector box lost vertical mapping");
    }
    Check(bottom_right && clipped, "Lower quadrant coordinates/clipping lost");
    Check(
        hm::pipeline::PrepareTiledInferenceBuffer(buffer, quadrants).ok() &&
            hm::pipeline::MergeTiledInferenceBuffer(buffer, quadrants).ok() && frame->num_obj_meta == 6,
        "Empty inference leaked quadrant metadata");
    gst_buffer_unref(buffer);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
