#pragma once

#include "hstream/src/libs/common/DecodedFrameSequenceMeta.h"

namespace hm {

// Plain metadata only. Interned URIs and integer timestamps survive removal of
// either input frame and copying between the CLI and independently linked plugin.
struct StitchingFramePair {
  DecodedFrameSequence left;
  DecodedFrameSequence right;
  GstClockTime timeline_pts{GST_CLOCK_TIME_NONE};
  uint32_t left_width{0};
  uint32_t left_height{0};
  uint32_t right_width{0};
  uint32_t right_height{0};
};

NvDsMetaType stitching_frame_pair_meta_type();
bool add_stitching_frame_pair_meta(NvDsFrameMeta* frame, const StitchingFramePair& pair) noexcept;
std::optional<StitchingFramePair> stitching_frame_pair(const NvDsFrameMeta* frame) noexcept;

} // namespace hm
