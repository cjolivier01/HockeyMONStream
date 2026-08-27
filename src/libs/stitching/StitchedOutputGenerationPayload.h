#pragma once

#include <nvdsmeta.h>

#include <memory>
#include <string>
#include <utility>

namespace hm::stitching {

class StitchedOutputGenerationPayload {
 public:
  explicit StitchedOutputGenerationPayload(
      std::string generation,
      std::string authorization_id = {},
      std::string scoreboard_property_value = {},
      std::string hugin_generation = {})
      : generation_(std::move(generation)),
        authorization_id_(std::move(authorization_id)),
        scoreboard_property_value_(std::move(scoreboard_property_value)),
        hugin_generation_(std::move(hugin_generation)) {}

  const std::string& generation() const {
    return generation_;
  }

  const std::string& authorization_id() const {
    return authorization_id_;
  }

  const std::string& scoreboard_property_value() const {
    return scoreboard_property_value_;
  }

  const std::string& hugin_generation() const {
    return hugin_generation_;
  }

 private:
  std::string generation_;
  std::string authorization_id_;
  std::string scoreboard_property_value_;
  std::string hugin_generation_;
};

inline NvDsMetaType stitched_output_generation_meta_type() {
  static const NvDsMetaType type = nvds_get_user_meta_type(const_cast<gchar*>("HSTREAM.STITCHED_OUTPUT_GENERATION"));
  return type;
}

inline gpointer copy_stitched_output_generation_meta(gpointer data, gpointer) noexcept {
  try {
    auto* user_meta = static_cast<NvDsUserMeta*>(data);
    const auto* payload =
        user_meta ? static_cast<const StitchedOutputGenerationPayload*>(user_meta->user_meta_data) : nullptr;
    return payload ? static_cast<gpointer>(new StitchedOutputGenerationPayload(*payload)) : nullptr;
  } catch (...) {
    return nullptr;
  }
}

inline void release_stitched_output_generation_meta(gpointer data, gpointer) noexcept {
  auto* user_meta = static_cast<NvDsUserMeta*>(data);
  if (!user_meta)
    return;
  delete static_cast<StitchedOutputGenerationPayload*>(user_meta->user_meta_data);
  user_meta->user_meta_data = nullptr;
}

inline const StitchedOutputGenerationPayload* find_stitched_output_generation_meta(
    const NvDsFrameMeta* frame_meta) noexcept {
  if (!frame_meta)
    return nullptr;
  for (const NvDsMetaList* item = frame_meta->frame_user_meta_list; item; item = item->next) {
    const auto* user_meta = static_cast<const NvDsUserMeta*>(item->data);
    if (user_meta && user_meta->base_meta.meta_type == stitched_output_generation_meta_type())
      return static_cast<const StitchedOutputGenerationPayload*>(user_meta->user_meta_data);
  }
  return nullptr;
}

inline bool add_stitched_output_generation_meta(
    NvDsFrameMeta* frame_meta,
    std::string generation,
    std::string authorization_id = {},
    std::string scoreboard_property_value = {},
    std::string hugin_generation = {}) noexcept {
  try {
    if (!frame_meta || !frame_meta->base_meta.batch_meta || generation.empty())
      return false;
    if (const auto* existing = find_stitched_output_generation_meta(frame_meta))
      return existing->generation() == generation && existing->authorization_id() == authorization_id &&
          existing->scoreboard_property_value() == scoreboard_property_value &&
          existing->hugin_generation() == hugin_generation;
    auto payload = std::make_unique<StitchedOutputGenerationPayload>(
        std::move(generation),
        std::move(authorization_id),
        std::move(scoreboard_property_value),
        std::move(hugin_generation));
    NvDsUserMeta* user_meta = nvds_acquire_user_meta_from_pool(frame_meta->base_meta.batch_meta);
    if (!user_meta)
      return false;
    user_meta->user_meta_data = payload.release();
    user_meta->base_meta.meta_type = stitched_output_generation_meta_type();
    user_meta->base_meta.copy_func = copy_stitched_output_generation_meta;
    user_meta->base_meta.release_func = release_stitched_output_generation_meta;
    nvds_add_user_meta_to_frame(frame_meta, user_meta);
    return true;
  } catch (...) {
    return false;
  }
}

} // namespace hm::stitching
