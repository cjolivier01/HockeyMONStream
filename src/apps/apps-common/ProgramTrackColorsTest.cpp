#include "hstream/src/apps/apps-common/ProgramTrackColors.h"
#include "hstream/src/apps/apps-common/deepstream_dsfieldmask.h"
#include "hstream/src/libs/common/TrackColorMeta.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>

GST_DEBUG_CATEGORY(NVDS_APP);

namespace {
using Producer = hm::gst::ProgramTrackColorProducer;

bool CheckCase(
    const char* name,
    bool root_plot_players,
    hm::gst::PluginProperties private_properties,
    hm::gst::PluginProperties public_properties,
    bool expected) {
  // Exercise the real builder's ordering: built-in setting, private-properties,
  // then properties.plugin-private-config (which replaces the entire string).
  auto config = std::make_unique<HmPlayCropperConfig>();
  std::strcpy(config->plugin_type, "playcropper");
  config->plot_player_tracking = root_plot_players;
  config->private_properties = std::move(private_properties);
  config->plugin_properties = std::move(public_properties);
  NvDsHmVideoPrepBin bin{};
  if (!create_hmplaycropper_bin(config.get(), &bin)) {
    std::cerr << name << ": could not construct cropper bin\n";
    if (bin.bin)
      gst_object_unref(bin.bin);
    return false;
  }

  bool okay = true;
  for (bool playtracker : {false, true}) {
    for (bool tracker : {false, true}) {
      const auto producer = hm::gst::ResolveProgramTrackColorProducer(bin.playcropper, playtracker, tracker);
      const auto wanted = !expected
          ? Producer::kNone
          : (playtracker ? Producer::kPlayTracker : (tracker ? Producer::kNativeTracker : Producer::kNone));
      okay &= producer == wanted;

      // Only the no-vpplaytracker path owns a fallback probe. Adding a preview
      // afterward must reuse that owner; neither disabled Program drawing nor
      // a vpplaytracker-owned request may allocate a native-tracker owner.
      GstElement* upstream = gst_element_factory_make("identity", nullptr);
      okay &= upstream != nullptr;
      if (upstream) {
        std::atomic<unsigned> preview_flags{0};
        const bool fallback = producer == Producer::kNativeTracker;
        okay &= hm::preview_overlay::ConfigureTrackColorProducer(upstream, fallback);
        gpointer owner = g_object_get_data(G_OBJECT(upstream), "hstream-track-color-producer");
        okay &= (owner != nullptr) == (expected && tracker && !playtracker);
        if (owner) {
          okay &= hm::preview_overlay::ConfigureTrackColorProducer(upstream, false, &preview_flags);
          okay &= owner == g_object_get_data(G_OBJECT(upstream), "hstream-track-color-producer");
        }
        gst_object_unref(upstream);
      }
    }
  }
  gst_object_unref(bin.bin);
  if (!okay)
    std::cerr << name << ": incorrect demand or upstream ownership\n";
  return okay;
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (argc != 2) {
    std::cerr << "Expected the built videoprep plugin path\n";
    return 1;
  }
  GError* error = nullptr;
  GstPlugin* plugin = gst_plugin_load_file(argv[1], &error);
  if (!plugin) {
    std::cerr << "Could not load videoprep: " << (error ? error->message : "unknown error") << '\n';
    g_clear_error(&error);
    return 1;
  }
  gst_object_unref(plugin);
  gst_registry_scan_path(gst_registry_get(), "/opt/nvidia/deepstream/deepstream/lib/gst-plugins");

  bool okay = hm::gst::ResolveProgramTrackColorProducer(nullptr, true, true) == Producer::kNone;
  okay &= CheckCase("default off", false, {}, {}, false);
  okay &= CheckCase("root on", true, {}, {}, true);
  okay &= CheckCase("private enables", false, {{"plot-player-tracking", "1"}}, {}, true);
  okay &= CheckCase("private disables", true, {{"plot-player-tracking", "0"}}, {}, false);
  okay &= CheckCase(
      "public enables",
      true,
      {{"plot-player-tracking", "0"}},
      {{"plugin-private-config", "plot-player-tracking=1"}},
      true);
  okay &= CheckCase(
      "public disables",
      false,
      {{"plot-player-tracking", "1"}},
      {{"plugin-private-config", "plot-player-tracking=0"}},
      false);
  okay &= CheckCase("public removes setting", true, {}, {{"plugin-private-config", "show-scoreboard=1"}}, false);
  okay &= CheckCase(
      "aliases and final numeric token",
      false,
      {},
      {{"plugin-private-config", "plot_player_tracking=0;plot-player_tracking= -2suffix"}},
      true);
  okay &= CheckCase(
      "boolean text uses atoi",
      true,
      {},
      {{"plugin-private-config", "plot-player-tracking=1;plot_player_tracking=true"}},
      false);
  okay &= CheckCase("empty value uses atoi", true, {}, {{"plugin-private-config", "plot-player-tracking="}}, false);
  okay &= CheckCase(
      "key whitespace is significant", false, {}, {{"plugin-private-config", " plot-player-tracking=1"}}, false);
  if (!okay)
    return 1;
  std::cout << "Final cropper properties and single upstream color-owner checks passed\n";
}
