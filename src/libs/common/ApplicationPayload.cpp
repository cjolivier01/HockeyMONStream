#include "hstream/src/libs/common/ApplicationPayload.h"

#if HAS_NVDS_CUSTOMUSERMETA
namespace hm {

const char* UserApplicationPayload::PayloadTypeName() const {
  return "HOCKEYMOM.USER.CUSTOM_META";
}

NvDsMetaType UserApplicationPayload::get_meta_type() {
  if (!meta_type_) {
    meta_type_ = nvds_get_user_meta_type((gchar*)PayloadTypeName());
  }
  return *meta_type_;
}

gpointer UserApplicationPayload::copy_user_meta(gpointer data, gpointer user_data) {
  NvDsUserMeta* user_meta = (NvDsUserMeta*)data;
  NVDS_CUSTOM_PAYLOAD* udata = (NVDS_CUSTOM_PAYLOAD*)user_meta->user_meta_data;
  NVDS_CUSTOM_PAYLOAD* dst_user_metadata = (NVDS_CUSTOM_PAYLOAD*)g_malloc0(sizeof(struct _NVDS_CUSTOM_PAYLOAD));
  dst_user_metadata->payload = (uint8_t*)((const UserApplicationPayload*)udata->payload)->CreateCopy();

  dst_user_metadata->payloadType = udata->payloadType;
  dst_user_metadata->payloadSize = udata->payloadSize;
  memcpy(dst_user_metadata->payload, udata->payload, udata->payloadSize);

  return (gpointer)dst_user_metadata;
}

/* release function set by user. "data" holds a pointer to NvDsUserMeta*/
void UserApplicationPayload::release_user_meta(gpointer data, gpointer user_data) {
  NvDsUserMeta* user_meta = (NvDsUserMeta*)data;
  if (user_meta->user_meta_data) {
    NVDS_CUSTOM_PAYLOAD* src_user_metadata = (NVDS_CUSTOM_PAYLOAD*)user_meta->user_meta_data;
    if (src_user_metadata->payload) {
      delete ((UserApplicationPayload*)(src_user_metadata->payload));
      src_user_metadata->payload = nullptr;
    }
    g_free(src_user_metadata);
    src_user_metadata = NULL;
  }
}

void UserApplicationPayload::add_to_frame(NVDS_CUSTOM_PAYLOAD* custom_payload, NvDsFrameMeta* frame_meta) {
  NvDsUserMeta* user_meta = NULL;
  NvDsMetaType user_meta_type = get_meta_type();
  assert(frame_meta->base_meta.batch_meta);
  /* Acquire NvDsUserMeta user meta from pool */
  user_meta = nvds_acquire_user_meta_from_pool(frame_meta->base_meta.batch_meta);

  /* Set NvDsUserMeta below */
  user_meta->user_meta_data = (void*)custom_payload;
  // g_print ("user meta data pointer = %p\n", user_meta->user_meta_data);
  user_meta->base_meta.meta_type = user_meta_type;
  user_meta->base_meta.copy_func = (NvDsMetaCopyFunc)copy_user_meta;
  user_meta->base_meta.release_func = (NvDsMetaReleaseFunc)release_user_meta;

  /* We want to add NvDsUserMeta to frame level */
  nvds_add_user_meta_to_frame(frame_meta, user_meta);
}

} // namespace hm
#endif
