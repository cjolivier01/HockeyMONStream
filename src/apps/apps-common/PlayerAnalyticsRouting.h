#pragma once

#include <cstdint>

#include "hstream/src/apps/apps-common/ProgramTrackColors.h"

namespace hm::gst {

// The tracked Stitched tee and fallback color/snapshot owner must be downstream
// of analytics, so semantic metadata exists before either consumer sees a frame.
// Pointers remain borrowed from the caller's pipeline.
inline GstElement* SelectTrackedPreviewSource(
    GstElement* playtracker,
    GstElement* analytics,
    GstElement* native_tracker) noexcept {
  return playtracker ? playtracker : (analytics ? analytics : native_tracker);
}

// Existing box demand remains authoritative. Semantic demand is already gated
// by compute through Config::drawing_layers(); preferences alone create no owner.
inline ProgramTrackColorProducer AddAnalyticsColorDemand(
    ProgramTrackColorProducer existing,
    uint32_t drawing_layers,
    bool playtracker_enabled,
    bool native_tracker_enabled) noexcept {
  if (existing != ProgramTrackColorProducer::kNone || !drawing_layers)
    return existing;
  return playtracker_enabled   ? ProgramTrackColorProducer::kPlayTracker
      : native_tracker_enabled ? ProgramTrackColorProducer::kNativeTracker
                               : ProgramTrackColorProducer::kNone;
}

} // namespace hm::gst
