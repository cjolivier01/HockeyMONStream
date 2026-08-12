#pragma once

#include <gst/base/gstbasetransform.h>

namespace hm::videoprep {

// Updates GstBaseTransform's negotiated src caps and forwards the CAPS event.
// Runtime-sized algorithms must use the base-transform API so its internal
// negotiation state stays synchronized with the event seen downstream.
bool update_runtime_output_caps(GstBaseTransform* transform, GstCaps* caps);

} // namespace hm::videoprep
