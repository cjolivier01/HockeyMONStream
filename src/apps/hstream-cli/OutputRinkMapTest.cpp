#include "OutputRinkMap.h"
#include <iostream>
#include <stdexcept>
#include "deepstream_app.h"

GST_DEBUG_CATEGORY(NVDS_APP);

namespace {
void Check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
GstElement* Identity(GstElement* bin) {
  auto* element = gst_element_factory_make("identity", nullptr);
  Check(element && gst_bin_add(GST_BIN(bin), element), "Cannot create routing fixture");
  return element;
}
void CheckOverlayBefore(GstElement* next) {
  auto* sink = gst_element_get_static_pad(next, "sink");
  auto* peer = gst_pad_get_peer(sink);
  auto* parent = peer ? gst_pad_get_parent_element(peer) : nullptr;
  Check(
      parent &&
          std::string(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(gst_element_get_factory(parent)))) == "hmrinkmap",
      "Encoder path is missing the rink-map element");
  gst_object_unref(parent);
  gst_object_unref(peer);
  gst_object_unref(sink);
}
} // namespace
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);
  try {
    // This is the graph shape produced by a Stitched-only exported job: a
    // stitcher and encoder routes, deliberately no inference or tracker bins.
    auto app = std::make_unique<AppCtx>();
    app->pipeline.pipeline = gst_pipeline_new(nullptr);
    app->pipeline.hmstitcher_bin.elem_hmstitcher = Identity(app->pipeline.pipeline);
    for (unsigned i = 0; i < 3; ++i) {
      auto& bin = app->pipeline.instance_bins[0].sink_bin.sub_bins[i];
      bin.bin = gst_bin_new(nullptr);
      gst_bin_add(GST_BIN(app->pipeline.pipeline), bin.bin);
      bin.queue = Identity(bin.bin);
      bin.transform = Identity(bin.bin);
      bin.cap_filter = Identity(bin.bin);
      bin.encoder = Identity(bin.bin);
      if (i == 2)
        bin.batch_demux = Identity(bin.bin);
      Check(gst_element_link_many(bin.queue, bin.transform, bin.cap_filter, nullptr), "Cannot link converter fixture");
      if (bin.batch_demux)
        Check(
            gst_element_link_many(bin.cap_filter, bin.batch_demux, bin.encoder, nullptr), "Cannot link demux fixture");
      else
        Check(gst_element_link(bin.cap_filter, bin.encoder), "Cannot link encoder fixture");
      app->config.sink_bin_sub_bin_config[i].encoder_config.enc_type =
          i == 1 ? NV_DS_ENCODER_TYPE_SW : NV_DS_ENCODER_TYPE_HW;
    }
    auto map = std::make_shared<hm::stitching::PlaybackRinkMap>();
    hm::rink_map::Configuration config{
        {true, hm::draw_display::RinkMiniMapPreferences("compact", "upper-right"), 8}, map, 0};
    const auto result = hm::InstallOutputRinkMap(*app, config);
    Check(result.ok(), result.ToString().c_str());
    Check(
        g_object_get_data(G_OBJECT(app->pipeline.pipeline), "hstream-output-rink-map"), "Lost output snapshot demand");
    auto& bins = app->pipeline.instance_bins[0].sink_bin.sub_bins;
    CheckOverlayBefore(bins[0].encoder);
    CheckOverlayBefore(bins[1].transform);
    CheckOverlayBefore(bins[2].batch_demux);
    gst_object_unref(app->pipeline.pipeline);
    app->pipeline.pipeline = nullptr;
    std::cout << "Stitched-only, hardware, software and demux output routing passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
