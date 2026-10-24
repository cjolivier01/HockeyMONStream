#pragma once

#include <gst/gst.h>
#include <gst/video/video-info.h>
#include <cstddef>
#include "absl/status/status.h"

namespace hm::program {
enum class EncodedCodec { kH264, kHevc };
// Install before starting the encoder, whose source must be Annex B access
// units. Enforce the actual input's matrix/range/transfer/primaries and centered
// chroma in SPS/VUI headers, including encoders that omit them. Only compressed
// access-unit bytes are mapped/copied; video pixels remain GPU-resident and
// slice payloads are never decoded or re-encoded. State belongs to the encoder
// source pad and is released with it. Recreate with a replaced encoder.
// Errors post a bus error and retire the filter. Packet bounds are explicit.
absl::Status InstallEncodedColorimetry(
    GstElement* encoder,
    EncodedCodec codec,
    const GstVideoInfo& input,
    size_t maximum_packet_bytes);
} // namespace hm::program
