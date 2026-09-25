#pragma once

#include <gst/gst.h>

namespace hm::gst {

enum class ProgramTrackColorProducer { kNone, kPlayTracker, kNativeTracker };

// Resolve once after the cropper's private and public properties have both been
// applied. A null cropper means there is no Program drawing consumer. Preview
// requests remain independent and may later reuse the selected upstream owner.
ProgramTrackColorProducer ResolveProgramTrackColorProducer(
    GstElement* cropper,
    bool playtracker_enabled,
    bool native_tracker_enabled);

} // namespace hm::gst
