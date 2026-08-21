#include "hstream/src/apps/apps-common/UriPlaylistDiscovery.h"

#include <iostream>

GST_DEBUG_CATEGORY(NVDS_APP);

int main() {
  using hm::uri_playlist_internal::discovery_has_usable_duration;

  bool ok = true;
  const auto expect = [&ok](bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAILED: " << message << '\n';
      ok = false;
    }
  };
  expect(discovery_has_usable_duration(GST_DISCOVERER_OK, GST_SECOND), "complete discovery should be usable");
  expect(
      discovery_has_usable_duration(GST_DISCOVERER_MISSING_PLUGINS, GST_SECOND),
      "an optional unsupported stream must not hide a valid container duration");
  expect(!discovery_has_usable_duration(GST_DISCOVERER_ERROR, GST_SECOND), "a failed discovery must remain unusable");
  expect(
      !discovery_has_usable_duration(GST_DISCOVERER_TIMEOUT, GST_SECOND), "a timed-out discovery must remain unusable");
  expect(
      !discovery_has_usable_duration(GST_DISCOVERER_MISSING_PLUGINS, GST_CLOCK_TIME_NONE),
      "missing duration must remain unusable");
  expect(!discovery_has_usable_duration(GST_DISCOVERER_MISSING_PLUGINS, 0), "zero duration must remain unusable");
  return ok ? 0 : 1;
}
