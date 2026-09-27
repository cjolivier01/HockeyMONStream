#pragma once

#include <gst/gst.h>

#include <string>

namespace hm::archive_watermark {

// Attach to the final NVMM color-conversion output, before batch demux and
// encoding. Its buffer is owned by this archive branch, not the stitched tee.
// `font_path` is empty in production, where the atlas picks an installed
// monospace TTF; tests point it at a missing file to exercise the failure.
bool Install(GstElement* caps_filter, int gpu_id, std::string font_path = {});

} // namespace hm::archive_watermark
