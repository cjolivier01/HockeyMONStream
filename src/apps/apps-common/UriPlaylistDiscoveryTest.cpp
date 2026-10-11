#include "hstream/src/apps/apps-common/UriPlaylistDiscovery.h"

#include <glib/gstdio.h>
#include <iostream>

GST_DEBUG_CATEGORY(NVDS_APP);

int main() {
  gst_init(nullptr, nullptr);
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

  // A failed probe must recover when a file arrives, and a cached duration must
  // not survive replacing or deleting that file at the same URI.
  gchar* directory = g_dir_make_tmp("uri-duration-test-XXXXXX", nullptr);
  if (!directory)
    return 1;
  const std::string path = std::string(directory) + "/sample.wav";
  gchar* uri = gst_filename_to_uri(path.c_str(), nullptr);
  const auto duration = [&] { return hm::uri_playlist_internal::discover_uri_playlist_duration(uri); };
  expect(duration() == GST_CLOCK_TIME_NONE, "a missing file must have no duration");
  for (unsigned seconds : {1, 2}) {
    const unsigned payload_size = seconds * 8000 * 2;
    std::string wav(44 + payload_size, '\0');
    const auto put = [&](unsigned at, unsigned value, unsigned bytes) {
      for (unsigned i = 0; i < bytes; ++i)
        wav[at + i] = static_cast<char>(value >> (8 * i));
    };
    wav.replace(0, 4, "RIFF");
    put(4, 36 + payload_size, 4);
    wav.replace(8, 8, "WAVEfmt ");
    put(16, 16, 4);
    put(20, 1, 2);
    put(22, 1, 2);
    put(24, 8000, 4);
    put(28, 16000, 4);
    put(32, 2, 2);
    put(34, 16, 2);
    wav.replace(36, 4, "data");
    put(40, payload_size, 4);
    expect(g_file_set_contents(path.c_str(), wav.data(), wav.size(), nullptr), "fixture must be written");
    expect(duration() == seconds * GST_SECOND, "discovery must observe the current file's duration");
    expect(duration() == seconds * GST_SECOND, "repeated discovery must preserve duration");
  }
  g_remove(path.c_str());
  expect(duration() == GST_CLOCK_TIME_NONE, "a deleted file must not reuse cached duration");
  g_free(uri);
  g_rmdir(directory);
  g_free(directory);
  return ok ? 0 : 1;
}
