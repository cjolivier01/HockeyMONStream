#include "hstream/src/libs/player_analytics/FrameMeta.h"

#include <cmath>
#include <memory>

namespace hm::player_analytics {
namespace {

using Payload = std::shared_ptr<const FrameResult>;

Payload* CopyHandle(const Payload* payload, FrameMetaFailureInjection injection) noexcept {
  try {
    if (injection == FrameMetaFailureInjection::kAllocation)
      throw std::bad_alloc();
    return payload ? new Payload(*payload) : nullptr;
  } catch (...) {
    return nullptr;
  }
}

gpointer Copy(gpointer data, gpointer) noexcept {
  const auto* meta = static_cast<const NvDsUserMeta*>(data);
  const auto* payload = meta ? static_cast<const Payload*>(meta->user_meta_data) : nullptr;
  return CopyHandle(payload, FrameMetaFailureInjection::kNone);
}

void Release(gpointer data, gpointer) noexcept {
  auto* meta = static_cast<NvDsUserMeta*>(data);
  if (meta) {
    delete static_cast<Payload*>(meta->user_meta_data);
    meta->user_meta_data = nullptr;
  }
}

bool Confidence(float value) {
  return std::isfinite(value) && value >= 0 && value <= 1;
}

bool Position(float value) {
  return std::isfinite(value) && std::abs(value) <= 1000000;
}

bool Times(uint64_t observed, uint64_t expires, uint64_t frame_pts) {
  return observed != kInvalidTime && observed <= frame_pts && expires != kInvalidTime && expires >= observed;
}

} // namespace

NvDsMetaType FrameResultMetaType() {
  static const auto type = nvds_get_user_meta_type(const_cast<gchar*>("HSTREAM.PLAYER_ANALYTICS.V1"));
  return type;
}

bool ValidateFrameResult(const FrameResult& result) noexcept {
  if (result.schema_version != FrameResult::kSchemaVersion || result.player_count > kMaximumTracks ||
      result.pts_ns == kInvalidTime || !Position(result.coordinate_width) || !Position(result.coordinate_height) ||
      result.coordinate_width <= 0 || result.coordinate_height <= 0)
    return false;
  for (size_t i = 0; i < result.player_count; ++i) {
    const auto& player = result.players[i];
    if (player.track_id == kUntrackedId || !Position(player.box.left) || !Position(player.box.top) ||
        !Position(player.box.width) || !Position(player.box.height) || player.box.width <= 0 ||
        player.box.height <= 0 || (player.color_slot != kNoColor && player.color_slot >= 32))
      return false;
    for (size_t j = 0; j < i; ++j)
      if (result.players[j].track_id == player.track_id)
        return false;
    if (player.has_pose) {
      if (player.pose_observed_at != result.pts_ns)
        return false;
      for (const auto& keypoint : player.pose)
        if (!Position(keypoint.x) || !Position(keypoint.y) || !Confidence(keypoint.confidence))
          return false;
    }
    if (player.jersey.text[0]) {
      const auto& text = player.jersey.text;
      if (text[2] != '\0' || text[0] < '0' || text[0] > '9' || (text[1] != '\0' && (text[1] < '0' || text[1] > '9')) ||
          !Confidence(player.jersey.confidence) || !std::isfinite(player.jersey.evidence) ||
          player.jersey.evidence < 0 || !Times(player.jersey.observed_at, player.jersey.expires_at, result.pts_ns))
        return false;
    }
    if (player.action.label != -1 &&
        (player.action.label < 0 || player.action.label >= 60 || !Confidence(player.action.confidence) ||
         !Times(player.action.observed_at, player.action.expires_at, result.pts_ns) ||
         player.action.window_start == kInvalidTime || player.action.window_start > player.action.observed_at))
      return false;
  }
  return true;
}

bool AttachFrameResult(NvDsFrameMeta* frame, const FrameResult& result, FrameMetaFailureInjection injection) noexcept {
  try {
    if (!frame || !frame->base_meta.batch_meta || FindFrameResult(frame) || !ValidateFrameResult(result))
      return false;
    if (frame->source_id != result.stream_id || frame->buf_pts != result.pts_ns ||
        static_cast<float>(frame->source_frame_width) != result.coordinate_width ||
        static_cast<float>(frame->source_frame_height) != result.coordinate_height)
      return false;
    if (injection == FrameMetaFailureInjection::kAllocation)
      throw std::bad_alloc();
    auto payload = std::make_unique<Payload>(std::make_shared<const FrameResult>(result));
    auto* meta = injection == FrameMetaFailureInjection::kPoolExhausted
        ? nullptr
        : nvds_acquire_user_meta_from_pool(frame->base_meta.batch_meta);
    if (!meta)
      return false;
    meta->user_meta_data = payload.release();
    meta->base_meta.meta_type = FrameResultMetaType();
    meta->base_meta.copy_func = Copy;
    meta->base_meta.release_func = Release;
    nvds_add_user_meta_to_frame(frame, meta);
    return true;
  } catch (...) {
    return false;
  }
}

bool FrameResultCopySucceedsForTest(const NvDsFrameMeta* frame, FrameMetaFailureInjection injection) noexcept {
  if (!frame)
    return false;
  for (const auto* item = frame->frame_user_meta_list; item; item = item->next) {
    const auto* meta = static_cast<const NvDsUserMeta*>(item->data);
    if (meta && meta->base_meta.meta_type == FrameResultMetaType()) {
      std::unique_ptr<Payload> copy(CopyHandle(static_cast<const Payload*>(meta->user_meta_data), injection));
      return static_cast<bool>(copy);
    }
  }
  return false;
}

const FrameResult* FindFrameResult(const NvDsFrameMeta* frame) noexcept {
  if (!frame)
    return nullptr;
  for (const auto* item = frame->frame_user_meta_list; item; item = item->next) {
    const auto* meta = static_cast<const NvDsUserMeta*>(item->data);
    if (meta && meta->base_meta.meta_type == FrameResultMetaType() && meta->user_meta_data)
      return static_cast<const Payload*>(meta->user_meta_data)->get();
  }
  return nullptr;
}

} // namespace hm::player_analytics
