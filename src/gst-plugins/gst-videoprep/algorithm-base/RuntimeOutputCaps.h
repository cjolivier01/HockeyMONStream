#pragma once

#include <gst/base/gstbasetransform.h>

namespace hm::videoprep {

enum class RuntimeOutputCapsUpdateResult {
  kUpdated,
  kCancelled,
  kFailed,
};

// Updates GstBaseTransform's negotiated src caps and forwards the CAPS event.
// Runtime-sized algorithms must use the base-transform API so its internal
// negotiation state stays synchronized with the event seen downstream.
// A flushing or unlinked src pad is a terminal pipeline-state transition, not
// a live negotiation failure.
RuntimeOutputCapsUpdateResult update_runtime_output_caps(GstBaseTransform* transform, GstCaps* caps);

} // namespace hm::videoprep
