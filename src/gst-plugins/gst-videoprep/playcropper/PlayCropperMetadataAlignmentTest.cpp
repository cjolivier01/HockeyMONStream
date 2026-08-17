#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/preputils.h"

#include <iostream>

#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerCtx.h"
#include "nvdsmeta.h"

int main() {
  NvDsBatchMeta* batch_meta = nvds_create_batch_meta(2);
  if (!batch_meta) {
    std::cerr << "Could not allocate batch metadata\n";
    return 1;
  }

  NvDsFrameMeta* tracked_frame = nvds_acquire_frame_meta_from_pool(batch_meta);
  NvDsFrameMeta* waiting_frame = nvds_acquire_frame_meta_from_pool(batch_meta);
  if (!tracked_frame || !waiting_frame) {
    std::cerr << "Could not allocate frame metadata\n";
    nvds_destroy_batch_meta(batch_meta);
    return 2;
  }
  nvds_add_frame_meta_to_batch(batch_meta, tracked_frame);
  nvds_add_frame_meta_to_batch(batch_meta, waiting_frame);

  NvDsObjectMeta* play_box = nvds_acquire_obj_meta_from_pool(batch_meta);
  if (!play_box) {
    std::cerr << "Could not allocate play-box metadata\n";
    nvds_destroy_batch_meta(batch_meta);
    return 3;
  }
  play_box->class_id = DsPlayTrackerInitParams::kPlayBoxClassIdBase;
  play_box->rect_params.left = 100;
  play_box->rect_params.top = 50;
  play_box->rect_params.width = 800;
  play_box->rect_params.height = 450;
  nvds_add_obj_meta_to_frame(tracked_frame, play_box, nullptr);

  const auto boxes = hm::get_object_boxes_by_frame(
      batch_meta, DsPlayTrackerInitParams::kPlayBoxClassIdBase, DsPlayTrackerInitParams::kPlayBoxClassIdBase);
  const bool aligned = boxes.size() == 2 && boxes[0].has_value() && !boxes[1].has_value() && boxes[0]->left == 100 &&
      boxes[0]->top == 50 && boxes[0]->right == 900 && boxes[0]->bottom == 500;

  nvds_destroy_batch_meta(batch_meta);
  if (!aligned) {
    std::cerr << "Mixed tracked/waiting batch did not preserve per-frame crop alignment\n";
    return 4;
  }
  return 0;
}
