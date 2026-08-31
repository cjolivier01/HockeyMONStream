#include "hstream/src/libs/common/DetectionSnapshotMeta.h"

#include <iostream>

int main() {
  NvDsBatchMeta* batch = nvds_create_batch_meta(1);
  NvDsFrameMeta* frame = batch ? nvds_acquire_frame_meta_from_pool(batch) : nullptr;
  NvDsObjectMeta* primary = batch ? nvds_acquire_obj_meta_from_pool(batch) : nullptr;
  NvDsObjectMeta* secondary = batch ? nvds_acquire_obj_meta_from_pool(batch) : nullptr;
  if (!batch || !frame || !primary || !secondary) {
    if (batch)
      nvds_destroy_batch_meta(batch);
    std::cerr << "Could not allocate DeepStream metadata\n";
    return 1;
  }

  nvds_add_frame_meta_to_batch(batch, frame);
  primary->unique_component_id = 1;
  primary->class_id = 0;
  primary->confidence = 0.75F;
  primary->detector_bbox_info.org_bbox_coords = NvBbox_Coords{10.0F, 20.0F, 30.0F, 40.0F};
  nvds_add_obj_meta_to_frame(frame, primary, nullptr);
  secondary->unique_component_id = 2;
  secondary->class_id = 7;
  secondary->confidence = 0.9F;
  secondary->detector_bbox_info.org_bbox_coords = NvBbox_Coords{50.0F, 60.0F, 70.0F, 80.0F};
  nvds_add_obj_meta_to_frame(frame, secondary, nullptr);

  const bool attached = hm::detection_snapshot::add_meta(batch, 1);
  const auto* snapshot = hm::detection_snapshot::find_meta(batch, frame);
  primary->detector_bbox_info.org_bbox_coords.left = 999.0F;
  const bool valid = attached && snapshot && snapshot->detections.size() == 1 &&
      snapshot->detections[0].left == 10.0F && snapshot->detections[0].top == 20.0F &&
      snapshot->detections[0].width == 30.0F && snapshot->detections[0].height == 40.0F &&
      snapshot->detections[0].score == 0.75F && snapshot->detections[0].class_id == 0 &&
      hm::detection_snapshot::add_meta(batch, 1) && hm::detection_snapshot::find_meta(batch, frame) == snapshot;
  nvds_destroy_batch_meta(batch);
  if (!valid) {
    std::cerr << "Primary detection snapshot was incomplete, mutable, or duplicated\n";
    return 2;
  }

  NvDsBatchMeta* empty_batch = nvds_create_batch_meta(1);
  NvDsFrameMeta* empty_frame = empty_batch ? nvds_acquire_frame_meta_from_pool(empty_batch) : nullptr;
  if (!empty_batch || !empty_frame) {
    if (empty_batch)
      nvds_destroy_batch_meta(empty_batch);
    std::cerr << "Could not allocate empty snapshot metadata\n";
    return 3;
  }
  nvds_add_frame_meta_to_batch(empty_batch, empty_frame);
  const bool empty_attached = hm::detection_snapshot::add_empty_meta(empty_batch);
  const auto* empty_snapshot = hm::detection_snapshot::find_meta(empty_batch, empty_frame);
  const bool empty_valid = empty_attached && empty_snapshot && empty_snapshot->detections.empty();
  nvds_destroy_batch_meta(empty_batch);
  if (!empty_valid) {
    std::cerr << "No-detector batch did not receive an explicit empty snapshot\n";
    return 4;
  }
  return 0;
}
