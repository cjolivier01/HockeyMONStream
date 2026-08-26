#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

#include <cstring>
#include <iostream>
#include <string>

#include "nvdsmeta.h"

int main() {
  NvDsBatchMeta* batch = nvds_create_batch_meta(1);
  if (!batch) {
    std::cerr << "FAIL: unable to allocate batch metadata\n";
    return 1;
  }
  NvDsFrameMeta* frame = nvds_acquire_frame_meta_from_pool(batch);
  if (!frame) {
    std::cerr << "FAIL: unable to allocate frame metadata\n";
    nvds_destroy_batch_meta(batch);
    return 1;
  }
  frame->base_meta.batch_meta = batch;
  const std::string generation = "hugin\npost-stitch-rotate-degrees:12.5\n";
  const bool added = hm::stitching::add_stitched_output_generation_meta(frame, generation);
  const bool duplicate_added = hm::stitching::add_stitched_output_generation_meta(frame, generation);
  const auto* attached = hm::stitching::find_stitched_output_generation_meta(frame);
  if (!added || !duplicate_added || !attached || attached->generation() != generation || !frame->frame_user_meta_list ||
      frame->frame_user_meta_list->next) {
    std::cerr << "FAIL: stitched-output generation metadata did not round-trip\n";
    nvds_destroy_batch_meta(batch);
    return 1;
  }

  auto* user_meta = static_cast<NvDsUserMeta*>(frame->frame_user_meta_list->data);
  gpointer copied_data = user_meta->base_meta.copy_func(user_meta, nullptr);
  const auto* copied_generation = static_cast<const hm::stitching::StitchedOutputGenerationPayload*>(copied_data);
  const bool copy_ok = copied_generation && copied_generation->generation() == generation;
  NvDsUserMeta copied_user_meta{};
  copied_user_meta.user_meta_data = copied_data;
  user_meta->base_meta.release_func(&copied_user_meta, nullptr);
  nvds_destroy_batch_meta(batch);
  if (!copy_ok) {
    std::cerr << "FAIL: copied stitched-output generation metadata was corrupted\n";
    return 1;
  }
  return 0;
}
