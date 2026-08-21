#pragma once

#include <gst/pbutils/pbutils.h>

namespace hm::uri_playlist_internal {

inline bool discovery_has_usable_duration(GstDiscovererResult result, GstClockTime duration) {
  const bool discovered_container = result == GST_DISCOVERER_OK || result == GST_DISCOVERER_MISSING_PLUGINS;
  return discovered_container && GST_CLOCK_TIME_IS_VALID(duration) && duration > 0;
}

} // namespace hm::uri_playlist_internal
