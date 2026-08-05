#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

#include <cstring>
#include <iostream>
#include <string>

#include "nvdsmeta.h"

int main() {
#ifdef HAS_NVDS_CUSTOMUSERMETA
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
  hm::stitching::StitchedOutputGenerationPayload::create_and_add<hm::stitching::StitchedOutputGenerationPayload>(
      frame, generation);
  const auto* attached = hm::UserApplicationPayload::get_payload<hm::stitching::StitchedOutputGenerationPayload>(frame);
  if (!attached || attached->generation() != generation || !frame->frame_user_meta_list) {
    std::cerr << "FAIL: stitched-output generation metadata did not round-trip\n";
    nvds_destroy_batch_meta(batch);
    return 1;
  }

  auto* user_meta = static_cast<NvDsUserMeta*>(frame->frame_user_meta_list->data);
  gpointer copied_data = user_meta->base_meta.copy_func(user_meta, nullptr);
  auto* copied_payload = static_cast<NVDS_CUSTOM_PAYLOAD*>(copied_data);
  const auto* copied_generation =
      reinterpret_cast<const hm::stitching::StitchedOutputGenerationPayload*>(copied_payload->payload);
  const bool copy_ok = copied_generation && copied_generation->generation() == generation;
  NvDsUserMeta copied_user_meta{};
  copied_user_meta.user_meta_data = copied_data;
  user_meta->base_meta.release_func(&copied_user_meta, nullptr);
  nvds_destroy_batch_meta(batch);
  if (!copy_ok) {
    std::cerr << "FAIL: copied stitched-output generation metadata was corrupted\n";
    return 1;
  }
#endif
  return 0;
}
