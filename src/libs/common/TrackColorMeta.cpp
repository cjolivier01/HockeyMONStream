#include "hstream/src/libs/common/TrackColorMeta.h"

#include <array>
#include <memory>
#include <string_view>

#include <gstnvdsmeta.h>

#include "hstream/src/libs/common/PreviewOverlayMeta.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

namespace hm::preview_overlay {

bool TrackColorState::Apply(NvDsFrameMeta* frame) noexcept {
  if (!frame)
    return false;
  try {
    auto found = streams_.find(frame->pad_index);
    if (found == streams_.end()) {
      if (streams_.size() >= kMaximumStreams)
        return false;
      found = streams_.try_emplace(frame->pad_index).first;
    }
    Stream& stream = found->second;
    const auto* generation = stitching::find_stitched_output_generation_meta(frame);
    const std::string_view current_generation = generation ? generation->generation() : std::string_view();
    if (stream.initialized &&
        (stream.width != frame->source_frame_width || stream.height != frame->source_frame_height ||
         stream.generation != current_generation || frame->frame_num < stream.frame_num)) {
      ++stream.epoch;
      stream.colors.Reset();
    }
    stream.initialized = true;
    stream.width = frame->source_frame_width;
    stream.height = frame->source_frame_height;
    stream.frame_num = frame->frame_num;
    if (stream.generation != current_generation)
      stream.generation = current_generation;

    std::array<uint64_t, player_analytics::kMaximumTracks> ids{};
    size_t count = 0;
    for (NvDsMetaList* item = frame->obj_meta_list; item && count < ids.size(); item = item->next) {
      const auto* object = static_cast<const NvDsObjectMeta*>(item->data);
      if (object && object->class_id == 0 && object->object_id != UNTRACKED_OBJECT_ID)
        ids[count++] = object->object_id;
    }
    stream.colors.Update(stream.epoch, frame->buf_pts, ids.data(), count);
    for (NvDsMetaList* item = frame->obj_meta_list; item; item = item->next) {
      auto* object = static_cast<NvDsObjectMeta*>(item->data);
      if (!object || object->class_id != 0)
        continue;
      const auto lease = stream.colors.Find(object->object_id);
      if (lease) {
        const auto& color = player_analytics::kTrackPalette[lease->slot];
        object->rect_params.border_color = {color.red, color.green, color.blue, color.alpha};
      } else {
        object->rect_params.border_color = {0.45, 0.45, 0.45, 1.0};
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

void TrackColorState::Reset() noexcept {
  streams_.clear();
}

namespace {
constexpr char kProducerKey[] = "hstream-track-color-producer";
struct Producer {
  TrackColorState colors;
  bool always_color{false};
  std::atomic<unsigned>* preview_flags{nullptr};
  bool failure_reported{false};
};

GstPadProbeReturn Produce(GstPad*, GstPadProbeInfo* info, gpointer data) noexcept {
  auto* producer = static_cast<Producer*>(data);
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    // FLUSH_STOP is serialized after old buffer processing; clearing state at
    // nonserialized FLUSH_START would race the streaming thread.
    const auto type = GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info));
    if (type == GST_EVENT_FLUSH_STOP || type == GST_EVENT_STREAM_START || type == GST_EVENT_EOS)
      producer->colors.Reset();
    return GST_PAD_PROBE_OK;
  }
  const unsigned flags = producer->preview_flags ? producer->preview_flags->load(std::memory_order_acquire) : 0;
  if (!producer->always_color && flags == 0)
    return GST_PAD_PROBE_OK;
  GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  NvDsBatchMeta* batch = buffer ? gst_buffer_get_nvds_batch_meta(buffer) : nullptr;
  if (!batch)
    return GST_PAD_PROBE_OK;
  bool failed = false;
  for (NvDsMetaList* item = batch->frame_meta_list; item; item = item->next) {
    auto* frame = static_cast<NvDsFrameMeta*>(item->data);
    if (!frame)
      continue;
    if (producer->always_color || (flags & 1U))
      failed |= !producer->colors.Apply(frame);
    if (flags)
      failed |= !add_selected_overlay_snapshot_meta(
          frame, (flags & 1U) != 0, (flags & 2U) != 0, (flags & 2U) ? frame->display_meta_list : nullptr);
  }
  if (failed && !producer->failure_reported) {
    producer->failure_reported = true;
    g_printerr("HSTREAM_PREVIEW_OVERLAY status=player-color-snapshot-unavailable\n");
  }
  return GST_PAD_PROBE_OK;
}
} // namespace

bool ConfigureTrackColorProducer(
    GstElement* upstream,
    bool color_players,
    std::atomic<unsigned>* preview_flags,
    const char* pad_name) noexcept {
  if (!upstream)
    return false;
  if (!color_players && !preview_flags)
    return true;
  try {
    auto* existing = static_cast<Producer*>(g_object_get_data(G_OBJECT(upstream), kProducerKey));
    if (existing) {
      existing->always_color |= color_players;
      if (preview_flags)
        existing->preview_flags = preview_flags;
      return true;
    }
    GstPad* pad = gst_element_get_static_pad(upstream, pad_name);
    if (!pad)
      return false;
    auto state = std::make_unique<Producer>();
    state->always_color = color_players;
    state->preview_flags = preview_flags;
    const gulong id = gst_pad_add_probe(
        pad,
        static_cast<GstPadProbeType>(
            GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH),
        Produce,
        state.get(),
        +[](gpointer value) noexcept { delete static_cast<Producer*>(value); });
    gst_object_unref(pad);
    if (!id)
      return false;
    // Non-owning lookup; the pad owns the producer and is retired with this
    // element. Configuration is only allowed before the graph starts.
    g_object_set_data(G_OBJECT(upstream), kProducerKey, state.release());
    return true;
  } catch (...) {
    return false;
  }
}

} // namespace hm::preview_overlay
