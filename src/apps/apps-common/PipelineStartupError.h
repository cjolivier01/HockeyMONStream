#pragma once

#include <gst/gst.h>

#include "absl/status/status.h"

namespace hm::gst {

// Call only after a failed startup state transition, before the bus watch is
// dispatched. Consume an already-queued error without waiting or pumping events;
// otherwise retain the caller's generic failure description.
absl::Status PipelineStartupFailure(GstElement* pipeline, const char* fallback);

} // namespace hm::gst
