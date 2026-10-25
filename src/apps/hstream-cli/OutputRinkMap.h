#pragma once
#include <filesystem>
#include "absl/status/status.h"
#include "hstream/src/gst-plugins/gst-rink-map/RinkMapOverlay.h"
class HmApp;
struct _AppCtx;
namespace hm {
// Install a prepared immutable map into existing output branches before READY.
absl::Status InstallOutputRinkMap(_AppCtx& app, const rink_map::Configuration& config);
absl::Status ConfigureOutputRinkMap(HmApp& app, const std::filesystem::path& game, bool preparation);
} // namespace hm
