#pragma once

#include <gst/gst.h>

/** Register HStream's full-batch-only DeepStream mux under the hstreamlosslessmux factory name. */
bool register_hstream_lossless_nvstreammux();

/** Whether this element is hstreamlosslessmux rather than DeepStream's own mux. */
bool is_hstream_lossless_nvstreammux(GstElement* element);
