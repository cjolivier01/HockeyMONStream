#include "OutputRinkMap.h"
#include <gstnvdsmeta.h>
#include "deepstream_app.h"
#include "hstream/src/gst-plugins/gst-rink-map/RinkMapOverlay.h"
#include "hstream/src/libs/common/PreviewOverlayMeta.h"
#include "hstream/src/libs/common/TrackColorMeta.h"

namespace hm {
namespace {
GstPadProbeReturn Snapshot(GstPad*, GstPadProbeInfo* info, gpointer user) noexcept {
  try {
    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    auto* batch = buffer ? gst_buffer_get_nvds_batch_meta(buffer) : nullptr;
    if (!batch)
      return GST_PAD_PROBE_OK;
    auto& colors = *static_cast<preview_overlay::TrackColorState*>(user);
    for (auto* node = batch->frame_meta_list; node; node = node->next) {
      auto* frame = static_cast<NvDsFrameMeta*>(node->data);
      if (!preview_overlay::find_overlay_snapshot_meta(frame)) {
        if (!colors.Apply(frame) || !preview_overlay::add_selected_overlay_snapshot_meta(frame, true, false, nullptr))
          g_printerr("HSTREAM_OUTPUT_RINK_MAP status=unavailable message=Cannot snapshot player positions\n");
      }
    }
  } catch (...) {
    g_printerr("HSTREAM_OUTPUT_RINK_MAP status=unavailable message=Player snapshot failed\n");
  }
  return GST_PAD_PROBE_OK;
}
} // namespace
absl::Status ConfigureOutputRinkMap(HmApp& app, const std::filesystem::path& game, bool preparation) {
  if (preparation)
    return absl::OkStatus();
  auto settings = rink_map::ResolveSettings(app.configurator().config());
  if (!settings.ok())
    return settings.status();
  if (!settings->enabled)
    return absl::OkStatus();
  bool any = false;
  for (auto& instance : app.pipeline.instance_bins)
    for (auto& bin : instance.sink_bin.sub_bins)
      any |= bin.encoder != nullptr;
  if (!any)
    return absl::OkStatus();
  auto map = stitching::LoadPlaybackRinkMap(game);
  if (!map.ok()) {
    g_printerr("HSTREAM_OUTPUT_RINK_MAP status=unavailable message=%s\n", map.status().ToString().c_str());
    return absl::OkStatus();
  }
  settings->options.home_is_dark = app.config.player_analytics_config.analytics.home_is_dark;
  g_object_set_data(G_OBJECT(app.pipeline.pipeline), "hstream-output-rink-map", GINT_TO_POINTER(1));
  if (auto* producer = app.pipeline.dsplaytracker_bin.elem_dsplaytracker) {
    unsigned flags = 0;
    g_object_get(producer, "preview-overlay-flags", &flags, nullptr);
    g_object_set(producer, "preview-overlay-flags", flags | 1U, nullptr);
  }
  // Before every tee/crop: freeze original stitched positions independently of
  // desktop preview flags. The output never relies on a visible-box selection.
  auto* upstream = app.pipeline.dsplaytracker_bin.bin;
  if (!upstream)
    upstream = app.pipeline.common_elements.tracker_bin.bin;
  if (!upstream)
    return absl::FailedPreconditionError("Output rink map requires the tracked Program path");
  auto* pad = gst_element_get_static_pad(upstream, "src");
  if (!pad)
    return absl::InternalError("Cannot find rink-map snapshot pad");
  gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, Snapshot, new preview_overlay::TrackColorState, [](gpointer p) {
    delete static_cast<preview_overlay::TrackColorState*>(p);
  });
  gst_object_unref(pad);
  for (auto& instance : app.pipeline.instance_bins) {
    for (unsigned i = 0; i < MAX_SINK_BINS; ++i) {
      auto& bin = instance.sink_bin.sub_bins[i];
      if (!bin.encoder)
        continue;
      const auto& encoder = app.config.sink_bin_sub_bin_config[i].encoder_config;
      // Software encoding's existing converter owns the only readback. Draw on
      // its raw GPU input; hardware paths draw after the final output resize.
      GstElement* next = encoder.enc_type == NV_DS_ENCODER_TYPE_SW ? bin.transform
          : bin.batch_demux                                        ? bin.batch_demux
                                                                   : bin.encoder;
      auto* sink = gst_element_get_static_pad(next, "sink");
      auto* peer = sink ? gst_pad_get_peer(sink) : nullptr;
      if (!peer) {
        if (sink)
          gst_object_unref(sink);
        return absl::InternalError("Missing encoder input link");
      }
      auto filter = rink_map::Create({*settings, *map, int(encoder.gpu_id)});
      if (!filter.ok()) {
        gst_object_unref(peer);
        gst_object_unref(sink);
        return filter.status();
      }
      gst_bin_add(GST_BIN(bin.bin), *filter);
      auto* input = gst_element_get_static_pad(*filter, "sink");
      auto* output = gst_element_get_static_pad(*filter, "src");
      gst_pad_unlink(peer, sink);
      const bool linked = gst_pad_link(peer, input) == GST_PAD_LINK_OK && gst_pad_link(output, sink) == GST_PAD_LINK_OK;
      gst_object_unref(input);
      gst_object_unref(output);
      gst_object_unref(peer);
      gst_object_unref(sink);
      if (!linked)
        return absl::InternalError("Cannot link output rink-map filter");
    }
  }
  return absl::OkStatus();
}
} // namespace hm
