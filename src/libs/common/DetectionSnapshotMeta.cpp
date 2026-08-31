#include "hstream/src/libs/common/DetectionSnapshotMeta.h"

#include <memory>

namespace hm::detection_snapshot {
namespace {

struct FrameSnapshot {
  guint source_id{0};
  gint frame_num{0};
  Snapshot snapshot;
};

struct BatchSnapshot {
  std::vector<FrameSnapshot> frames;
};

gpointer copy_meta(gpointer data, gpointer) noexcept {
  try {
    const auto* user_meta = static_cast<const NvDsUserMeta*>(data);
    const auto* snapshot = user_meta ? static_cast<const BatchSnapshot*>(user_meta->user_meta_data) : nullptr;
    return snapshot ? static_cast<gpointer>(new BatchSnapshot(*snapshot)) : nullptr;
  } catch (...) {
    return nullptr;
  }
}

void release_meta(gpointer data, gpointer) noexcept {
  auto* user_meta = static_cast<NvDsUserMeta*>(data);
  if (!user_meta)
    return;
  delete static_cast<BatchSnapshot*>(user_meta->user_meta_data);
  user_meta->user_meta_data = nullptr;
}

} // namespace

NvDsMetaType meta_type() {
  static const NvDsMetaType type = nvds_get_user_meta_type(const_cast<gchar*>("HSTREAM.DETECTION_SNAPSHOT"));
  return type;
}

namespace {

const BatchSnapshot* find_batch_meta(const NvDsBatchMeta* batch_meta) noexcept {
  if (!batch_meta)
    return nullptr;
  for (const NvDsMetaList* item = batch_meta->batch_user_meta_list; item; item = item->next) {
    const auto* user_meta = static_cast<const NvDsUserMeta*>(item->data);
    if (user_meta && user_meta->base_meta.meta_type == meta_type())
      return static_cast<const BatchSnapshot*>(user_meta->user_meta_data);
  }
  return nullptr;
}

} // namespace

const Snapshot* find_meta(const NvDsBatchMeta* batch_meta, const NvDsFrameMeta* frame_meta) noexcept {
  if (!frame_meta)
    return nullptr;
  const BatchSnapshot* batch_snapshot = find_batch_meta(batch_meta);
  if (!batch_snapshot)
    return nullptr;
  for (const FrameSnapshot& frame_snapshot : batch_snapshot->frames) {
    if (frame_snapshot.source_id == frame_meta->source_id && frame_snapshot.frame_num == frame_meta->frame_num)
      return &frame_snapshot.snapshot;
  }
  return nullptr;
}

namespace {

bool add_meta_impl(NvDsBatchMeta* batch_meta, const gint* primary_component_id) noexcept {
  try {
    if (!batch_meta)
      return false;
    if (find_batch_meta(batch_meta))
      return true;

    auto batch_snapshot = std::make_unique<BatchSnapshot>();
    for (const NvDsMetaList* frame_item = batch_meta->frame_meta_list; frame_item; frame_item = frame_item->next) {
      const auto* frame_meta = static_cast<const NvDsFrameMeta*>(frame_item->data);
      if (!frame_meta)
        continue;
      FrameSnapshot frame_snapshot;
      frame_snapshot.source_id = frame_meta->source_id;
      frame_snapshot.frame_num = frame_meta->frame_num;
      if (primary_component_id) {
        frame_snapshot.snapshot.detections.reserve(frame_meta->num_obj_meta);
        for (const NvDsMetaList* item = frame_meta->obj_meta_list; item; item = item->next) {
          const auto* object = static_cast<const NvDsObjectMeta*>(item->data);
          if (!object || object->unique_component_id != *primary_component_id)
            continue;
          NvBbox_Coords box = object->detector_bbox_info.org_bbox_coords;
          if (box.width <= 0.0F || box.height <= 0.0F) {
            box.left = object->rect_params.left;
            box.top = object->rect_params.top;
            box.width = object->rect_params.width;
            box.height = object->rect_params.height;
          }
          frame_snapshot.snapshot.detections.push_back(
              Detection{box.left, box.top, box.width, box.height, object->confidence, object->class_id});
        }
      }
      batch_snapshot->frames.push_back(std::move(frame_snapshot));
    }

    NvDsUserMeta* user_meta = nvds_acquire_user_meta_from_pool(batch_meta);
    if (!user_meta)
      return false;
    user_meta->user_meta_data = batch_snapshot.release();
    user_meta->base_meta.meta_type = meta_type();
    user_meta->base_meta.copy_func = copy_meta;
    user_meta->base_meta.release_func = release_meta;
    nvds_add_user_meta_to_batch(batch_meta, user_meta);
    return true;
  } catch (...) {
    return false;
  }
}

} // namespace

bool add_meta(NvDsBatchMeta* batch_meta, gint primary_component_id) noexcept {
  return add_meta_impl(batch_meta, &primary_component_id);
}

bool add_empty_meta(NvDsBatchMeta* batch_meta) noexcept {
  return add_meta_impl(batch_meta, nullptr);
}

} // namespace hm::detection_snapshot
