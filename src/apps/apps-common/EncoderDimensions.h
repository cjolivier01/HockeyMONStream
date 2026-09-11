#pragma once

#include <gst/gst.h>

#include <optional>
#include <utility>

namespace hm {

struct EncoderDimensionLimits {
  guint min_width{2};
  guint min_height{2};
  guint max_width{0};
  guint max_height{0};
  // NVENC reports a per-frame budget in 16x16 macroblocks. Zero means no extra budget.
  guint max_macroblocks{0};
};

std::optional<std::pair<guint, guint>> fit_encoder_dimensions(
    guint width,
    guint height,
    const EncoderDimensionLimits& limits);

// Query the selected GPU/codec, without relying on nvv4l2enc's unbounded pad templates.
std::optional<EncoderDimensionLimits> query_encoder_dimensions(GstElement* encoder, bool hevc, guint gpu_id);

// Resize only inside this branch's GPU converter, when the actual upstream CAPS arrive.
bool install_encoder_dimension_limit(
    GstElement* converter,
    GstElement* caps_filter,
    const EncoderDimensionLimits& limits,
    bool main10 = false);

} // namespace hm
