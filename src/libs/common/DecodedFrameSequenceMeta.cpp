#include "hstream/src/libs/common/DecodedFrameSequenceMeta.h"

#include "gstnvdsmeta.h"

namespace hm {
namespace {

constexpr char kDecodedFrameSequenceMetaApiName[] = "GstHmDecodedFrameSequenceMetaAPI";
constexpr char kDecodedFrameSequenceMetaImplementationName[] = "GstHmDecodedFrameSequenceMeta";
constexpr gsize kRegistrationFailed = 1;

struct GstHmDecodedFrameSequenceMeta {
  GstMeta meta;
  guint source_id;
  guint64 sequence;
};

GType decoded_frame_sequence_meta_api_get_type() {
  static gsize type = 0;
  static const gchar* tags[] = {"hstream", "decoded-frame-sequence", nullptr};
  if (g_once_init_enter(&type)) {
    // This implementation is linked into both hstream-cli and the separately loaded videoprep plugin. Their local
    // g_once guards do not protect GStreamer's process-global type registry from a duplicate registration. Reuse the
    // process-global type when another image registered it first; a failed registration can also mean that the other
    // image won the race between the lookup and registration calls.
    GType registered = g_type_from_name(kDecodedFrameSequenceMetaApiName);
    if (registered == G_TYPE_INVALID) {
      registered = gst_meta_api_type_register(kDecodedFrameSequenceMetaApiName, tags);
    }
    if (registered == G_TYPE_INVALID) {
      registered = g_type_from_name(kDecodedFrameSequenceMetaApiName);
    }
    g_once_init_leave(&type, registered == G_TYPE_INVALID ? kRegistrationFailed : registered);
  }
  return type == kRegistrationFailed ? G_TYPE_INVALID : static_cast<GType>(type);
}

gboolean decoded_frame_sequence_meta_init(GstMeta* meta, gpointer /*params*/, GstBuffer* /*buffer*/) {
  auto* sequence_meta = reinterpret_cast<GstHmDecodedFrameSequenceMeta*>(meta);
  sequence_meta->source_id = 0;
  sequence_meta->sequence = 0;
  return TRUE;
}

const GstMetaInfo* decoded_frame_sequence_meta_get_info();

gboolean decoded_frame_sequence_meta_transform(
    GstBuffer* destination,
    GstMeta* meta,
    GstBuffer* /*source*/,
    GQuark type,
    gpointer /*data*/) {
  if (!GST_META_TRANSFORM_IS_COPY(type)) {
    return FALSE;
  }
  const auto* source_meta = reinterpret_cast<const GstHmDecodedFrameSequenceMeta*>(meta);
  auto* destination_meta = reinterpret_cast<GstHmDecodedFrameSequenceMeta*>(
      gst_buffer_add_meta(destination, decoded_frame_sequence_meta_get_info(), nullptr));
  if (!destination_meta) {
    return FALSE;
  }
  destination_meta->source_id = source_meta->source_id;
  destination_meta->sequence = source_meta->sequence;
  return TRUE;
}

const GstMetaInfo* decoded_frame_sequence_meta_get_info() {
  static gsize info = 0;
  if (g_once_init_enter(&info)) {
    const GType api = decoded_frame_sequence_meta_api_get_type();
    const GstMetaInfo* registered = gst_meta_get_info(kDecodedFrameSequenceMetaImplementationName);
    if (!registered && api != G_TYPE_INVALID) {
      registered = gst_meta_register(
          api,
          kDecodedFrameSequenceMetaImplementationName,
          sizeof(GstHmDecodedFrameSequenceMeta),
          decoded_frame_sequence_meta_init,
          nullptr,
          decoded_frame_sequence_meta_transform);
    }
    if (!registered) {
      registered = gst_meta_get_info(kDecodedFrameSequenceMetaImplementationName);
    }
    g_once_init_leave(&info, registered ? reinterpret_cast<gsize>(registered) : kRegistrationFailed);
  }
  return info == kRegistrationFailed ? nullptr : reinterpret_cast<const GstMetaInfo*>(info);
}

const GstHmDecodedFrameSequenceMeta* find_sequence_meta(GstBuffer* buffer) {
  if (!buffer) {
    return nullptr;
  }
  gpointer state = nullptr;
  while (GstMeta* meta = gst_buffer_iterate_meta(buffer, &state)) {
    if (meta->info && meta->info->api == decoded_frame_sequence_meta_api_get_type()) {
      return reinterpret_cast<const GstHmDecodedFrameSequenceMeta*>(meta);
    }
  }
  return nullptr;
}

} // namespace

bool add_decoded_frame_sequence_meta(GstBuffer* buffer, guint source_id, uint64_t sequence) {
  if (!buffer || !gst_buffer_is_writable(buffer)) {
    return false;
  }
  const GstMetaInfo* info = decoded_frame_sequence_meta_get_info();
  if (!info) {
    return false;
  }
  auto* meta = reinterpret_cast<GstHmDecodedFrameSequenceMeta*>(gst_buffer_add_meta(buffer, info, nullptr));
  if (!meta) {
    return false;
  }
  meta->source_id = source_id;
  meta->sequence = sequence;
  return true;
}

std::optional<DecodedFrameSequence> decoded_frame_sequence(GstBuffer* buffer) {
  const GstHmDecodedFrameSequenceMeta* meta = find_sequence_meta(buffer);
  if (!meta) {
    return std::nullopt;
  }
  return DecodedFrameSequence{meta->source_id, meta->sequence};
}

std::optional<DecodedFrameSequence> decoded_frame_sequence(const NvDsFrameMeta* frame_meta) {
  if (!frame_meta) {
    return std::nullopt;
  }
  for (NvDsUserMetaList* item = frame_meta->frame_user_meta_list; item != nullptr; item = item->next) {
    const auto* user_meta = static_cast<const NvDsUserMeta*>(item->data);
    if (!user_meta || user_meta->base_meta.meta_type != static_cast<NvDsMetaType>(NVDS_BUFFER_GST_AS_FRAME_USER_META)) {
      continue;
    }
    const auto* meta = find_sequence_meta(static_cast<GstBuffer*>(user_meta->user_meta_data));
    if (meta) {
      return DecodedFrameSequence{meta->source_id, meta->sequence};
    }
  }
  return std::nullopt;
}

} // namespace hm
