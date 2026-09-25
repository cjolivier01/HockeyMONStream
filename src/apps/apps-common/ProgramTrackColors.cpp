#include "hstream/src/apps/apps-common/ProgramTrackColors.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>

namespace hm::gst {

ProgramTrackColorProducer ResolveProgramTrackColorProducer(
    GstElement* cropper,
    bool playtracker_enabled,
    bool native_tracker_enabled) {
  if (!cropper || (!playtracker_enabled && !native_tracker_enabled))
    return ProgramTrackColorProducer::kNone;

  gchar* value = nullptr;
  g_object_get(G_OBJECT(cropper), "plugin-private-config", &value, nullptr);
  std::unique_ptr<gchar, decltype(&g_free)> owned_value(value, &g_free);
  std::istringstream config(value ? value : "");
  bool plot_players = false;
  std::string pair;
  while (std::getline(config, pair, ';')) {
    // Match VideoPrepPriv::SetPrivateConfig and PlayCropperPriv::SetProperty:
    // exactly one '=', no key trimming, '_' aliases, atoi, and last token wins.
    const auto separator = pair.find('=');
    if (separator == std::string::npos || separator != pair.rfind('='))
      continue;
    std::string key = pair.substr(0, separator);
    std::replace(key.begin(), key.end(), '_', '-');
    if (key == "plot-player-tracking")
      plot_players = std::atoi(pair.c_str() + separator + 1) != 0;
  }
  if (!plot_players)
    return ProgramTrackColorProducer::kNone;
  return playtracker_enabled ? ProgramTrackColorProducer::kPlayTracker : ProgramTrackColorProducer::kNativeTracker;
}

} // namespace hm::gst
