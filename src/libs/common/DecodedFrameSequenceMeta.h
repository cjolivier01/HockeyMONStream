#pragma once

#include <gst/gst.h>

#include <cstdint>
#include <optional>

#include "nvdsmeta.h"

namespace hm {

struct DecodedFrameSequence {
  guint source_id{0};
  uint64_t sequence{0};
};

/**
 * Adds a decoder-output sequence number that survives nvvideoconvert and nvstreammux. The sequence starts at zero and
 * never resets at URI chapter boundaries.
 */
bool add_decoded_frame_sequence_meta(GstBuffer* buffer, guint source_id, uint64_t sequence);

/** Returns the decoder-output sequence copied by nvstreammux into this frame's user metadata, when present. */
std::optional<DecodedFrameSequence> decoded_frame_sequence(const NvDsFrameMeta* frame_meta);

} // namespace hm
