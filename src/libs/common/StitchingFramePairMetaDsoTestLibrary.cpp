#include "hstream/src/libs/common/StitchingFramePairMeta.h"

extern "C" bool stitching_frame_pair_dso_add(NvDsFrameMeta* frame, const hm::StitchingFramePair* pair) {
  return pair && hm::add_stitching_frame_pair_meta(frame, *pair);
}

extern "C" bool stitching_frame_pair_dso_read(const NvDsFrameMeta* frame, hm::StitchingFramePair* pair) {
  const auto value = hm::stitching_frame_pair(frame);
  if (!value || !pair)
    return false;
  *pair = *value;
  return true;
}
