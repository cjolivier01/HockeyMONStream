#include "hstream/src/apps/apps-common/PreviewRouteGraph.h"
#include "hstream/src/apps/apps-common/deepstream_sinks.h"

#include <cstdlib>
#include <iostream>

GST_DEBUG_CATEGORY(NVDS_APP);

namespace {
void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::abort();
  }
}
unsigned Sources(GstElement* element) {
  auto* iterator = gst_element_iterate_src_pads(element);
  GValue item = G_VALUE_INIT;
  unsigned count = 0;
  while (gst_iterator_next(iterator, &item) == GST_ITERATOR_OK) {
    ++count;
    g_value_reset(&item);
  }
  if (G_VALUE_TYPE(&item))
    g_value_unset(&item);
  gst_iterator_free(iterator);
  return count;
}
} // namespace

void Test(bool realtime, bool calibration) {
  set_render_playback_realtime(realtime, calibration);
  const auto paced = realtime && !calibration;
  auto* target = gst_bin_new("shared-target");
  gst_object_ref_sink(target); // The target wrapper retains an independent ref.
  auto* endpoint = gst_element_factory_make("fakesink", "stable-audio-endpoint");
  Check(target && endpoint && gst_bin_add(GST_BIN(target), endpoint), "Could not create target stand-in");
  NvDsSinkSubBinConfig config{};
  config.enable = TRUE;
  config.type = NV_DS_SINK_WEBRTC;
  config.encoder_config.preview_bin = target;
  config.encoder_config.preview_endpoint = endpoint;
  NvDsSinkBin output{};
  Check(create_sink_bin(1, &config, &output, 0), "Shared target sink construction failed");
  Check(
      output.num_bins == 1 && output.sub_bins[0].bin == target && output.sub_bins[0].sink == endpoint,
      "Sink/audio ownership did not retain the supplied target");
  Check(Sources(output.tee) == unsigned(paced), "Shared target allocated an unexpected Program branch");
  auto* clock = gst_bin_get_by_name(GST_BIN(output.bin), "playback_clock");
  Check(bool(clock) == paced, "WebRTC pacing ignored playback or calibration selection");
  if (clock)
    gst_object_unref(clock);
  auto* pipeline = gst_pipeline_new("owner");
  Check(gst_bin_add(GST_BIN(pipeline), output.bin), "Could not parent output");
  auto* input = gst_element_get_static_pad(endpoint, "sink");
  auto* ingress = gst_element_factory_make("identity", "program-ingress");
  auto* queue = gst_element_factory_make("queue", "program-queue");
  auto* drain = gst_element_factory_make("identity", "program-drain");
  std::string error;
  auto attachment = hm::preview::RouteAttachment::Create(
      {hm::preview::RouteTap::Kind::kExistingTee, output.bin, output.tee}, {ingress, queue, drain}, input, &error);
  Check(attachment && attachment->Commit({}), "Could not attach the real Program tap to the shared consumer");
  Check(Sources(output.tee) == 1U + unsigned(paced), "Program acquired an unexpected consumer branch");
  attachment.reset();
  gst_object_unref(input);
  gst_object_unref(pipeline);
  Check(
      !GST_OBJECT_PARENT(target) && GST_OBJECT_PARENT(endpoint) == GST_OBJECT(target),
      "Graph teardown invalidated the target owner's stable endpoint");
  gst_object_unref(target);
}

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);
  Test(false, false);
  Test(true, false);
  Test(true, true);
  set_render_playback_realtime(FALSE);
  return 0;
}
