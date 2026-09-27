#pragma once

#include <gst/gst.h>

namespace hm::archive_watermark {

// Attach to the final NVMM color-conversion output, before batch demux and
// encoding. Its buffer is owned by this archive branch, not the stitched tee.
bool Install(GstElement* caps_filter, int gpu_id);

} // namespace hm::archive_watermark
