#include "hstream/src/libs/common/TrackColorMeta.h"

#include <gstnvdsmeta.h>

#include <iostream>
#include <vector>

#include "hstream/src/libs/common/PreviewOverlayMeta.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

namespace {
bool Same(const NvOSD_ColorParams& a, const NvOSD_ColorParams& b) {
  return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
}

struct Frame {
  NvDsBatchMeta* batch{nvds_create_batch_meta(1)};
  NvDsFrameMeta* meta{nvds_acquire_frame_meta_from_pool(batch)};
  std::vector<NvDsObjectMeta*> objects;
  explicit Frame(uint64_t pts, const std::vector<uint64_t>& ids, const char* generation = "generation-a") {
    nvds_add_frame_meta_to_batch(batch, meta);
    meta->source_frame_width = 4096;
    meta->source_frame_height = 2048;
    meta->buf_pts = pts;
    meta->frame_num = pts / 10000000;
    for (uint64_t id : ids) {
      auto* object = nvds_acquire_obj_meta_from_pool(batch);
      object->class_id = 0;
      object->object_id = id;
      object->rect_params = {};
      object->rect_params.width = 10;
      object->rect_params.height = 20;
      nvds_add_obj_meta_to_frame(meta, object, nullptr);
      objects.push_back(object);
    }
    hm::stitching::add_stitched_output_generation_meta(meta, generation);
  }
  ~Frame() {
    if (batch)
      nvds_destroy_batch_meta(batch);
  }

  GstBuffer* Buffer() {
    GstBuffer* buffer = gst_buffer_new();
    batch->max_frames_in_batch = 1;
    auto* attached =
        gst_buffer_add_nvds_meta(buffer, batch, nullptr, nvds_batch_meta_copy_func, nvds_batch_meta_release_func);
    attached->meta_type = NVDS_BATCH_GST_META;
    batch = nullptr;
    return buffer;
  }
};

bool CheckProducer() {
  GstElement* element = gst_element_factory_make("identity", "color-producer-test");
  GstPad* source = gst_element_get_static_pad(element, "src");
  GstPad* sink = gst_pad_new("sink", GST_PAD_SINK);
  gst_pad_set_chain_function(
      sink, +[](GstPad*, GstObject*, GstBuffer* buffer) {
        gst_buffer_unref(buffer);
        return GST_FLOW_OK;
      });
  gst_pad_set_active(source, TRUE);
  gst_pad_set_active(sink, TRUE);
  gst_pad_link(source, sink);
  gst_pad_push_event(source, gst_event_new_stream_start("track-colors"));
  GstSegment segment;
  gst_segment_init(&segment, GST_FORMAT_TIME);
  gst_pad_push_event(source, gst_event_new_segment(&segment));

  std::atomic<unsigned> flags{0};
  bool okay = hm::preview_overlay::ConfigureTrackColorProducer(element, false) &&
      !g_object_get_data(G_OBJECT(element), "hstream-track-color-producer") &&
      hm::preview_overlay::ConfigureTrackColorProducer(element, false, &flags);
  gpointer owner = g_object_get_data(G_OBJECT(element), "hstream-track-color-producer");
  auto push = [&](Frame& frame) {
    GstBuffer* buffer = frame.Buffer();
    // Keep one reference for metadata assertions after the synchronous probe.
    const auto flow = gst_pad_push(source, gst_buffer_ref(buffer));
    return std::pair<GstBuffer*, bool>{buffer, flow == GST_FLOW_OK};
  };
  Frame disabled(10000000, {99});
  auto [disabled_buffer, disabled_flow] = push(disabled);
  okay &= disabled_flow && !hm::preview_overlay::find_overlay_snapshot_meta(disabled.meta) &&
      disabled.objects[0]->rect_params.border_color.alpha == 0;
  gst_buffer_unref(disabled_buffer);

  flags.store(1);
  Frame enabled(20000000, {99});
  auto [enabled_buffer, enabled_flow] = push(enabled);
  const auto assigned = enabled.objects[0]->rect_params.border_color;
  const auto* snapshot = hm::preview_overlay::find_overlay_snapshot_meta(enabled.meta);
  okay &= enabled_flow && snapshot && snapshot->player_rects.size() == 1 && assigned.alpha == 1;
  gst_buffer_unref(enabled_buffer);

  // Reconfiguration combines independent encoded/debug and preview requests;
  // it must neither attach another owner nor recolor the same track twice.
  okay &= hm::preview_overlay::ConfigureTrackColorProducer(element, true) &&
      owner == g_object_get_data(G_OBJECT(element), "hstream-track-color-producer");
  flags.store(0);
  Frame encoded(30000000, {99});
  auto [encoded_buffer, encoded_flow] = push(encoded);
  okay &= encoded_flow && !hm::preview_overlay::find_overlay_snapshot_meta(encoded.meta) &&
      Same(encoded.objects[0]->rect_params.border_color, assigned);
  gst_buffer_unref(encoded_buffer);

  gst_pad_push_event(source, gst_event_new_flush_start());
  gst_pad_push_event(source, gst_event_new_flush_stop(TRUE));
  gst_pad_push_event(source, gst_event_new_segment(&segment));
  Frame reset(40000000, {100});
  auto [reset_buffer, reset_flow] = push(reset);
  okay &= reset_flow && Same(reset.objects[0]->rect_params.border_color, assigned);
  gst_buffer_unref(reset_buffer);
  gst_pad_unlink(source, sink);
  gst_pad_set_active(source, FALSE);
  gst_pad_set_active(sink, FALSE);
  gst_object_unref(source);
  gst_object_unref(sink);
  gst_object_unref(element);
  return okay;
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  hm::preview_overlay::TrackColorState colors;
  const uint64_t large_id = (uint64_t{1} << 40) + 7;
  Frame first(10000000, {large_id, large_id + 32, UNTRACKED_OBJECT_ID});
  if (!colors.Apply(first.meta) ||
      Same(first.objects[0]->rect_params.border_color, first.objects[1]->rect_params.border_color)) {
    std::cerr << "Distinct tracked players that collide modulo 32 need different colors\n";
    return 1;
  }
  const auto first_color = first.objects[0]->rect_params.border_color;
  const auto second_color = first.objects[1]->rect_params.border_color;
  if (!hm::preview_overlay::add_selected_overlay_snapshot_meta(first.meta, true, false, nullptr))
    return 2;
  const auto* snapshot = hm::preview_overlay::find_overlay_snapshot_meta(first.meta);
  if (!snapshot || snapshot->player_rects.size() != 2 ||
      !((Same(snapshot->player_rects[0].border_color, first_color) &&
         Same(snapshot->player_rects[1].border_color, second_color)) ||
        (Same(snapshot->player_rects[1].border_color, first_color) &&
         Same(snapshot->player_rects[0].border_color, second_color)))) {
    std::cerr << "Immutable snapshot lost shared player colors or included an untracked object\n";
    return 3;
  }
  // Later ordinary metadata mutations cannot change the Stitched tee's snapshot.
  const auto saved_snapshot_color = snapshot->player_rects[0].border_color;
  first.objects[0]->rect_params.border_color = {0, 0, 0, 0};
  if (!Same(snapshot->player_rects[0].border_color, saved_snapshot_color))
    return 4;

  Frame reordered(20000000, {large_id + 32, large_id});
  if (!colors.Apply(reordered.meta) || !Same(reordered.objects[0]->rect_params.border_color, second_color) ||
      !Same(reordered.objects[1]->rect_params.border_color, first_color)) {
    std::cerr << "A change in metadata order changed track color identity\n";
    return 5;
  }
  Frame changed(30000000, {large_id + 32}, "generation-b");
  hm::preview_overlay::TrackColorState fresh;
  Frame reference(30000000, {large_id + 32}, "generation-b");
  if (!colors.Apply(changed.meta) || !fresh.Apply(reference.meta) ||
      !Same(changed.objects[0]->rect_params.border_color, reference.objects[0]->rect_params.border_color)) {
    std::cerr << "A changed stitched generation retained old color leases\n";
    return 6;
  }
  colors.Reset();
  Frame reset(10000000, {large_id});
  if (!colors.Apply(reset.meta) || !Same(reset.objects[0]->rect_params.border_color, first_color))
    return 7;
  if (!CheckProducer()) {
    std::cerr << "Requested color producer failed off-path, shared ownership, or flush checks\n";
    return 8;
  }
  std::cout << "Track color metadata identity and immutable snapshots passed\n";
}
