// Opt-in hardware integration: pass native NvDCF base YAML and a ReID overlay
// pointing to the real prebuilt reference engine for the current GPU/SDK.
#include "hstream/src/apps/apps-common/HStreamLosslessMux.h"
#include "hstream/src/apps/apps-common/deepstream_tracker.h"

#include <gst/gst.h>
#include <gstnvdsmeta.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <iostream>

GST_DEBUG_CATEGORY(NVDS_APP);

namespace {
std::atomic<unsigned> frames{0};
std::atomic<unsigned> tracked{0};
std::atomic<unsigned> exported_embeddings{0};

GstPadProbeReturn add_detection(GstPad*, GstPadProbeInfo* info, gpointer) {
  auto* batch = gst_buffer_get_nvds_batch_meta(GST_PAD_PROBE_INFO_BUFFER(info));
  if (!batch)
    return GST_PAD_PROBE_OK;
  for (NvDsMetaList* item = batch->frame_meta_list; item; item = item->next) {
    auto* frame = static_cast<NvDsFrameMeta*>(item->data);
    frame->bInferDone = TRUE;
    auto* object = nvds_acquire_obj_meta_from_pool(batch);
    if (!object)
      continue;
    object->unique_component_id = 1;
    object->class_id = 0;
    object->object_id = UNTRACKED_OBJECT_ID;
    object->confidence = 0.99F;
    object->rect_params.left = 200;
    object->rect_params.top = 80;
    object->rect_params.width = 100;
    object->rect_params.height = 240;
    object->detector_bbox_info.org_bbox_coords = {200, 80, 100, 240};
    std::strncpy(object->obj_label, "player", MAX_LABEL_SIZE - 1);
    nvds_add_obj_meta_to_frame(frame, object, nullptr);
  }
  return GST_PAD_PROBE_OK;
}

GstPadProbeReturn inspect_output(GstPad*, GstPadProbeInfo* info, gpointer) {
  auto* batch = gst_buffer_get_nvds_batch_meta(GST_PAD_PROBE_INFO_BUFFER(info));
  if (!batch)
    return GST_PAD_PROBE_OK;
  for (NvDsMetaList* item = batch->batch_user_meta_list; item; item = item->next) {
    const auto* meta = static_cast<const NvDsUserMeta*>(item->data);
    if (meta->base_meta.meta_type == NVDS_TRACKER_BATCH_REID_META)
      ++exported_embeddings;
  }
  for (NvDsMetaList* item = batch->frame_meta_list; item; item = item->next) {
    ++frames;
    auto* frame = static_cast<NvDsFrameMeta*>(item->data);
    for (NvDsMetaList* object_item = frame->obj_meta_list; object_item; object_item = object_item->next) {
      const auto* object = static_cast<const NvDsObjectMeta*>(object_item->data);
      if (object->class_id == 0 && object->object_id != UNTRACKED_OBJECT_ID)
        ++tracked;
      for (NvDsMetaList* user = object->obj_user_meta_list; user; user = user->next) {
        const auto* meta = static_cast<const NvDsUserMeta*>(user->data);
        if (meta->base_meta.meta_type == NVDS_TRACKER_OBJ_REID_META)
          ++exported_embeddings;
      }
    }
  }
  return GST_PAD_PROBE_OK;
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: tracker_reid_native_test NVDCF_BASE_YAML REID_OVERLAY_YAML\n";
    return 2;
  }
  gst_init(&argc, &argv);
  if (!register_hstream_lossless_nvstreammux())
    return 1;
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "tracker-reid-test", 0, "Native tracker ReID test");
  NvDsTrackerConfig config{};
  config.enable = TRUE;
  config.width = 640;
  config.height = 384;
  config.ll_lib_file = const_cast<char*>("/opt/nvidia/deepstream/deepstream/lib/libnvds_nvmultiobjecttracker.so");
  config.ll_config_file = argv[1];
  config.reid_enable = TRUE;
  config.reid_config_file = argv[2];
  config.user_meta_pool_size = 32;
  NvDsTrackerBin tracker{};
  if (!create_tracking_bin(&config, &tracker))
    return 1;
  gchar* generated = nullptr;
  g_object_get(tracker.tracker, "ll-config-file", &generated, nullptr);
  const std::filesystem::path generated_path(generated ? generated : "");
  g_free(generated);
  if (generated_path.empty() || !std::filesystem::exists(generated_path)) {
    std::cerr << "Missing owned runtime tracker config\n";
    gst_object_unref(tracker.bin);
    return 1;
  }

  GError* error = nullptr;
  GstElement* pipeline = gst_parse_launch(
      "videotestsrc num-buffers=48 pattern=ball ! "
      "video/x-raw,format=RGBA,width=640,height=384,framerate=30/1 ! "
      "nvvideoconvert ! video/x-raw(memory:NVMM),format=RGBA ! mux.sink_0 "
      "hstreamlosslessmux name=mux batch-size=1 ! "
      "identity name=before fakesink name=after sync=false",
      &error);
  if (!pipeline || error) {
    std::cerr << (error ? error->message : "Cannot construct GPU source") << '\n';
    if (error)
      g_error_free(error);
    if (pipeline)
      gst_object_unref(pipeline);
    gst_object_unref(tracker.bin);
    return 1;
  }
  auto* before = gst_bin_get_by_name(GST_BIN(pipeline), "before");
  auto* after = gst_bin_get_by_name(GST_BIN(pipeline), "after");
  if (!gst_bin_add(GST_BIN(pipeline), tracker.bin) || !gst_element_link_many(before, tracker.bin, after, nullptr)) {
    std::cerr << "Cannot link native tracker\n";
    gst_object_unref(before);
    gst_object_unref(after);
    gst_object_unref(pipeline);
    return 1;
  }
  GstPad* pad = gst_element_get_static_pad(before, "src");
  gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, add_detection, nullptr, nullptr);
  gst_object_unref(pad);
  pad = gst_element_get_static_pad(after, "sink");
  gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, inspect_output, nullptr, nullptr);
  gst_object_unref(pad);
  gst_object_unref(before);
  gst_object_unref(after);
  GWeakRef tracker_lifetime;
  g_weak_ref_init(&tracker_lifetime, G_OBJECT(tracker.tracker));
  GstBus* bus = gst_element_get_bus(pipeline);
  bool passed = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
  GstMessage* message = passed
      ? gst_bus_timed_pop_filtered(
            bus, 45 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR))
      : nullptr;
  passed = passed && message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
  if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    gchar* debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    std::cerr << error->message << "\n" << (debug ? debug : "") << '\n';
    g_error_free(error);
    g_free(debug);
  }
  if (message)
    gst_message_unref(message);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  passed = passed && std::filesystem::exists(generated_path);
  // Failed NULL->READY starts can leave error messages holding tracker refs;
  // there is no READY->NULL transition to flush those automatically.
  gst_bus_set_flushing(bus, TRUE);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  // Native startup failure can release helper-thread references asynchronously.
  // The generated file must follow actual object destruction, not state=NULL.
  for (int attempt = 0; attempt < 1000; ++attempt) {
    auto* remaining = g_weak_ref_get(&tracker_lifetime);
    if (!remaining)
      break;
    g_object_unref(remaining);
    g_usleep(1000);
  }
  g_weak_ref_clear(&tracker_lifetime);
  passed =
      passed && !std::filesystem::exists(generated_path) && frames == 48 && tracked > 0 && exported_embeddings == 0;
  std::cout << "Native ReID frames=" << frames << " tracked=" << tracked
            << " exported-embeddings=" << exported_embeddings
            << " generated-config-removed=" << !std::filesystem::exists(generated_path) << '\n';
  return passed ? 0 : 1;
}
