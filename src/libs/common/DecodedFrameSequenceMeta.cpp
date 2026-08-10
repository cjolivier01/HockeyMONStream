#include "hstream/src/libs/common/DecodedFrameSequenceMeta.h"

#include "gstnvdsmeta.h"

namespace hm {
namespace {

constexpr char kDecodedFrameSequenceMetaApiName[] = "GstHmDecodedFrameSequenceMetaAPI";
constexpr char kDecodedFrameSequenceMetaImplementationName[] = "GstHmDecodedFrameSequenceMeta";
constexpr char kRegistrationMutexKey[] = "hstream-decoded-frame-sequence-meta-registration-mutex";
constexpr gsize kRegistrationFailed = 1;
constexpr gint64 kRegistrationPublicationTimeoutMicros = G_TIME_SPAN_SECOND;

struct GstHmDecodedFrameSequenceMeta {
  GstMeta meta;
  guint source_id;
  guint64 sequence;
};

GRecMutex* process_global_registration_mutex() {
  GstRegistry* registry = gst_registry_get();
  const GQuark key = g_quark_from_static_string(kRegistrationMutexKey);
  auto* mutex = static_cast<GRecMutex*>(g_object_get_qdata(G_OBJECT(registry), key));
  if (mutex) {
    return mutex;
  }

  auto* candidate = g_new0(GRecMutex, 1);
  g_rec_mutex_init(candidate);
  GDestroyNotify old_destroy = nullptr;
  if (g_object_replace_qdata(G_OBJECT(registry), key, nullptr, candidate, nullptr, &old_destroy)) {
    // GstRegistry lives for the process lifetime. Deliberately keep this tiny mutex with it so a plugin can safely
    // unload without leaving the registry with a destroy callback into that plugin's former address space.
    return candidate;
  }

  g_rec_mutex_clear(candidate);
  g_free(candidate);
  return static_cast<GRecMutex*>(g_object_get_qdata(G_OBJECT(registry), key));
}

class ProcessGlobalRegistrationLock {
 public:
  ProcessGlobalRegistrationLock() : mutex_(process_global_registration_mutex()) {
    g_rec_mutex_lock(mutex_);
  }
  ~ProcessGlobalRegistrationLock() {
    g_rec_mutex_unlock(mutex_);
  }

  ProcessGlobalRegistrationLock(const ProcessGlobalRegistrationLock&) = delete;
  ProcessGlobalRegistrationLock& operator=(const ProcessGlobalRegistrationLock&) = delete;

 private:
  GRecMutex* mutex_;
};

bool is_compatible_api(GType api) {
  return api != G_TYPE_INVALID && gst_meta_api_type_has_tag(api, g_quark_from_static_string("hstream")) &&
      gst_meta_api_type_has_tag(api, g_quark_from_static_string("decoded-frame-sequence"));
}

GType decoded_frame_sequence_meta_api_get_type() {
  static gsize type = 0;
  static const gchar* tags[] = {"hstream", "decoded-frame-sequence", nullptr};
  if (g_once_init_enter(&type)) {
    // This implementation is linked into both hstream-cli and the separately loaded videoprep plugin. Serialize their
    // local g_once initializers with a mutex attached to GStreamer's process-global registry, then reuse whichever
    // process-global API registration wins.
    ProcessGlobalRegistrationLock lock;
    GType registered = g_type_from_name(kDecodedFrameSequenceMetaApiName);
    if (registered == G_TYPE_INVALID) {
      registered = gst_meta_api_type_register(kDecodedFrameSequenceMetaApiName, tags);
    }
    if (registered == G_TYPE_INVALID) {
      registered = g_type_from_name(kDecodedFrameSequenceMetaApiName);
    }
    if (!is_compatible_api(registered)) {
      GST_ERROR("Existing decoded-frame sequence metadata API is incompatible");
      registered = G_TYPE_INVALID;
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

bool is_compatible_meta_info(const GstMetaInfo* info, GType api) {
  return info && info->api == api && info->size == sizeof(GstHmDecodedFrameSequenceMeta) && info->init_func &&
      info->transform_func && g_strcmp0(g_type_name(info->type), kDecodedFrameSequenceMetaImplementationName) == 0;
}

const GstMetaInfo* wait_for_published_meta_info() {
  const gint64 deadline = g_get_monotonic_time() + kRegistrationPublicationTimeoutMicros;
  const GstMetaInfo* info = nullptr;
  while (!(info = gst_meta_get_info(kDecodedFrameSequenceMetaImplementationName)) &&
         g_get_monotonic_time() < deadline) {
    g_thread_yield();
    g_usleep(1000);
  }
  return info;
}

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
  const GstMetaInfo* info = decoded_frame_sequence_meta_get_info();
  if (!info) {
    return FALSE;
  }
  auto* destination_meta =
      reinterpret_cast<GstHmDecodedFrameSequenceMeta*>(gst_buffer_add_meta(destination, info, nullptr));
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
    ProcessGlobalRegistrationLock lock;
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
    if (!registered && g_type_from_name(kDecodedFrameSequenceMetaImplementationName) != G_TYPE_INVALID) {
      // An independently linked image or external registrar can publish the implementation GType just before it
      // publishes GstMetaInfo. Do not cache that transient loser state as a permanent registration failure.
      registered = wait_for_published_meta_info();
    }
    if (!is_compatible_meta_info(registered, api)) {
      if (registered) {
        GST_ERROR("Existing decoded-frame sequence metadata implementation is ABI-incompatible");
      }
      registered = nullptr;
    }
    g_once_init_leave(&info, registered ? reinterpret_cast<gsize>(registered) : kRegistrationFailed);
  }
  return info == kRegistrationFailed ? nullptr : reinterpret_cast<const GstMetaInfo*>(info);
}

const GstHmDecodedFrameSequenceMeta* find_sequence_meta(GstBuffer* buffer) {
  if (!buffer) {
    return nullptr;
  }
  const GstMetaInfo* expected_info = decoded_frame_sequence_meta_get_info();
  if (!expected_info) {
    return nullptr;
  }
  gpointer state = nullptr;
  while (GstMeta* meta = gst_buffer_iterate_meta(buffer, &state)) {
    if (meta->info == expected_info) {
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
