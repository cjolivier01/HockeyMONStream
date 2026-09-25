#include "hstream/src/libs/player_analytics/FrameMeta.h"

#include <iostream>
#include <limits>
#include <memory>

namespace pa = hm::player_analytics;

int main() {
  auto input = std::make_unique<pa::FrameResult>();
  input->pts_ns = 10;
  input->coordinate_width = 2000;
  input->coordinate_height = 1000;
  input->player_count = 1;
  input->players[0].track_id = (1ULL << 50) + 123;
  input->players[0].box = {1, 2, 30, 70};
  input->players[0].jersey.text = {'0', '7', '\0'};
  input->players[0].jersey.observed_at = 9;
  input->players[0].jersey.expires_at = 20;
  NvDsBatchMeta* batch = nvds_create_batch_meta(1);
  auto* frame = batch ? nvds_acquire_frame_meta_from_pool(batch) : nullptr;
  if (!frame)
    return 1;
  frame->buf_pts = input->pts_ns;
  frame->source_frame_width = 2000;
  frame->source_frame_height = 1000;
  nvds_add_frame_meta_to_batch(batch, frame);
  if (pa::AttachFrameResult(frame, *input, pa::FrameMetaFailureInjection::kAllocation) ||
      pa::AttachFrameResult(frame, *input, pa::FrameMetaFailureInjection::kPoolExhausted) || pa::FindFrameResult(frame))
    return 10;
  frame->buf_pts = 9;
  if (pa::AttachFrameResult(frame, *input))
    return 11;
  frame->buf_pts = input->pts_ns;
  if (!pa::AttachFrameResult(frame, *input) || pa::AttachFrameResult(frame, *input))
    return 2;
  if (!pa::FrameResultCopySucceedsForTest(frame, pa::FrameMetaFailureInjection::kNone) ||
      pa::FrameResultCopySucceedsForTest(frame, pa::FrameMetaFailureInjection::kAllocation))
    return 12;
  const auto* result = pa::FindFrameResult(frame);
  input->players[0].jersey.text = {'9', '\0', '\0'};
  if (!result || result->players[0].jersey.text[0] != '0')
    return 3;
  auto* batch_a = nvds_create_batch_meta(1);
  auto* batch_b = nvds_create_batch_meta(1);
  auto* frame_a = batch_a ? nvds_acquire_frame_meta_from_pool(batch_a) : nullptr;
  auto* frame_b = batch_b ? nvds_acquire_frame_meta_from_pool(batch_b) : nullptr;
  if (!frame_a || !frame_b)
    return 4;
  nvds_add_frame_meta_to_batch(batch_a, frame_a);
  nvds_add_frame_meta_to_batch(batch_b, frame_b);
  nvds_copy_frame_meta(frame, frame_a);
  nvds_copy_frame_meta(frame, frame_b);
  if (pa::FindFrameResult(frame_a) != result || pa::FindFrameResult(frame_b) != result)
    return 13;
  nvds_destroy_batch_meta(batch);
  nvds_destroy_batch_meta(batch_a);
  // The earlier pools must not retire immutable data owned by a surviving copy.
  const auto* remaining = pa::FindFrameResult(frame_b);
  if (!remaining || remaining->players[0].jersey.text[1] != '7')
    return 5;
  nvds_destroy_batch_meta(batch_b);
  input->player_count = pa::kMaximumTracks + 1;
  if (pa::ValidateFrameResult(*input))
    return 6;
  input->player_count = 1;
  input->players[0].has_pose = true;
  input->players[0].pose_observed_at = 9;
  if (pa::ValidateFrameResult(*input))
    return 7; // Old poses cannot be painted as a fresh same-frame observation.
  input->players[0].pose_observed_at = input->pts_ns;
  input->players[0].pose[0].x = std::numeric_limits<float>::quiet_NaN();
  if (pa::ValidateFrameResult(*input))
    return 8;
  return 0;
}
