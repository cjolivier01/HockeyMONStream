#pragma once

#include "deepstream/sources/includes/nvdscustomusermeta.h"

#include <cassert>
#include <optional>
#include <type_traits>

namespace hm {

enum HmPayloadType : long {
  HM_PAYLOAD_TYPE_PLAY_TRACKER = NVDS_START_USER_META + 8192,
};

struct UserApplicationPayload {
  virtual ~UserApplicationPayload() = default;

  virtual UserApplicationPayload* CreateCopy() const = 0;

  void add_to_frame(NVDS_CUSTOM_PAYLOAD* custom_payload, NvDsFrameMeta* frame_meta);

  template <typename T, typename... Args>
  static NVDS_CUSTOM_PAYLOAD* create(Args&&... args);

  template <typename T, typename... Args>
  static T* create_and_add(NvDsFrameMeta* frame_meta, Args&&... args);

  static gpointer copy_user_meta(gpointer data, gpointer user_data);

  /* release function set by user. "data" holds a pointer to NvDsUserMeta*/
  static void release_user_meta(gpointer data, gpointer user_data);

  // Assumption is that there is only one of this type
  template <typename T>
  static const T* get_payload(const NvDsFrameMeta* frame_meta);

 protected:
  virtual const char* PayloadTypeName() const;

  virtual NvDsMetaType get_meta_type();

 private:
  std::optional<NvDsMetaType> meta_type_;
};

template <typename T, typename... Args>
inline NVDS_CUSTOM_PAYLOAD* create(Args&&... args) {
  NVDS_CUSTOM_PAYLOAD* dst_user_metadata = (NVDS_CUSTOM_PAYLOAD*)g_malloc0(sizeof(struct _NVDS_CUSTOM_PAYLOAD));
  auto payload = new T(std::forward<Args>(args)...);
  // Makes sure it's a derivative
  static_assert(std::is_base_of<UserApplicationPayload, T>::value);
  dst_user_metadata->payloadType = payload->PayloadType();
  // Nonya business
  dst_user_metadata->payloadSize = sizeof(T);
  dst_user_metadata->payload = (uint8_t*)payload;
  return dst_user_metadata;
}

template <typename T, typename... Args>
inline T* UserApplicationPayload::create_and_add(NvDsFrameMeta* frame_meta, Args&&... args) {
  NVDS_CUSTOM_PAYLOAD* dst_user_metadata = (NVDS_CUSTOM_PAYLOAD*)g_malloc0(sizeof(struct _NVDS_CUSTOM_PAYLOAD));
  auto payload = new T(std::forward<Args>(args)...);
  // Makes sure it's a derivative
  static_assert(std::is_base_of<UserApplicationPayload, T>::value);
  dst_user_metadata->payloadType = T::PayloadSubType();
  // Nonya business
  dst_user_metadata->payloadSize = sizeof(T);
  dst_user_metadata->payload = (uint8_t*)payload;
  payload->add_to_frame(dst_user_metadata, frame_meta);
  return payload;
}

// Assumption is that there is only one of this type
template <typename T>
inline const T* UserApplicationPayload::get_payload(const NvDsFrameMeta* frame_meta) {
  for (const NvDsUserMetaList* user_meta_list = frame_meta->frame_user_meta_list; user_meta_list != nullptr;
       user_meta_list = user_meta_list->next) {
    const NvDsUserMeta* user_meta = (const NvDsUserMeta*)user_meta_list->data;
    const NVDS_CUSTOM_PAYLOAD* src_user_metadata = (const NVDS_CUSTOM_PAYLOAD*)user_meta->user_meta_data;
    if (src_user_metadata->payloadType == T::PayloadSubType()) {
      return (const T*)src_user_metadata->payload;
    }
  }
  return nullptr;
}

} // namespace hm
