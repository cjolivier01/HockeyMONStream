#include "hstream/src/libs/common/StitchingFramePairMeta.h"

#include <cstring>
#include <type_traits>

namespace hm {
namespace {

static_assert(std::is_trivially_copyable_v<StitchingFramePair>);

gpointer copy_pair(gpointer data, gpointer) noexcept {
  const auto* meta = static_cast<const NvDsUserMeta*>(data);
  if (!meta || !meta->user_meta_data)
    return nullptr;
  gpointer copy = g_try_malloc(sizeof(StitchingFramePair));
  if (copy)
    std::memcpy(copy, meta->user_meta_data, sizeof(StitchingFramePair));
  return copy;
}

void release_pair(gpointer data, gpointer) noexcept {
  auto* meta = static_cast<NvDsUserMeta*>(data);
  if (meta) {
    g_free(meta->user_meta_data);
    meta->user_meta_data = nullptr;
  }
}

} // namespace

NvDsMetaType stitching_frame_pair_meta_type() {
  // DeepStream's registry is process-global, unlike C++ statics in separately
  // linked images. Version the name so a future layout cannot alias this one.
  static const NvDsMetaType type = nvds_get_user_meta_type(const_cast<gchar*>("HSTREAM.STITCHING_FRAME_PAIR.V1"));
  return type;
}

bool add_stitching_frame_pair_meta(NvDsFrameMeta* frame, const StitchingFramePair& pair) noexcept {
  if (!frame || !frame->base_meta.batch_meta || stitching_frame_pair(frame))
    return false;
  gpointer data = g_try_malloc(sizeof(pair));
  if (!data)
    return false;
  NvDsUserMeta* meta = nvds_acquire_user_meta_from_pool(frame->base_meta.batch_meta);
  if (!meta) {
    g_free(data);
    return false;
  }
  std::memcpy(data, &pair, sizeof(pair));
  meta->user_meta_data = data;
  meta->base_meta.meta_type = stitching_frame_pair_meta_type();
  meta->base_meta.copy_func = copy_pair;
  meta->base_meta.release_func = release_pair;
  nvds_add_user_meta_to_frame(frame, meta);
  return true;
}

std::optional<StitchingFramePair> stitching_frame_pair(const NvDsFrameMeta* frame) noexcept {
  if (!frame)
    return std::nullopt;
  for (const NvDsMetaList* item = frame->frame_user_meta_list; item; item = item->next) {
    const auto* meta = static_cast<const NvDsUserMeta*>(item->data);
    if (meta && meta->base_meta.meta_type == stitching_frame_pair_meta_type() && meta->user_meta_data)
      return *static_cast<const StitchingFramePair*>(meta->user_meta_data);
  }
  return std::nullopt;
}

} // namespace hm
