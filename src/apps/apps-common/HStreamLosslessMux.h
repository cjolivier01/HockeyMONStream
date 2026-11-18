#pragma once

/** Register HStream's full-batch-only DeepStream mux under the hstreamlosslessmux factory name. */
bool register_hstream_lossless_nvstreammux();

/**
 * Whether the source bin built for the current pipeline selected
 * hstreamlosslessmux. Only that element carries nvstreammux_p010_caps.patch,
 * so it is also the answer to whether the mux can pass 10-bit buffers.
 * create_multi_source_bin() sets this each time it runs.
 */
bool hstream_lossless_nvstreammux_selected();
void set_hstream_lossless_nvstreammux_selected(bool selected);
