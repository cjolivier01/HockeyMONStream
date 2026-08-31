#pragma once

#include "absl/status/status.h"

#include <gst/gst.h>

namespace hm::videoprep {

// Posts a non-cancellation output failure to the owning pipeline bus. Returning
// false means no fatal message was posted (for example, because the status is
// OK, cancellation was requested, or the element is unavailable).
bool post_fatal_output_error(GstElement* element, const absl::Status& status);

} // namespace hm::videoprep
