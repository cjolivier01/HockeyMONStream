#include "PreviewTransportFrames.h"

#include <algorithm>
#include <cstring>

namespace hm::preview {
bool H264AccessUnitHasIdr(const std::uint8_t* bytes, std::size_t size) {
  if (!bytes)
    return false;
  for (std::size_t index = 0; index + 3 < size; ++index) {
    if (bytes[index] || bytes[index + 1])
      continue;
    std::size_t header = index + 3;
    if (bytes[index + 2] == 0 && bytes[index + 3] == 1)
      ++header;
    else if (bytes[index + 2] != 1)
      continue;
    if (header < size && !(bytes[header] & 0x80) && (bytes[header] & 0x1f) == 5)
      return true;
  }
  return false;
}

TransportFrames::TransportFrames(std::size_t capacity) : capacity_(capacity) {}

bool TransportFrames::SetPeer(std::uint64_t peer) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!peer || peer <= peer_)
    return false;
  peer_ = peer;
  frames_.clear();
  need_keyframe_ = true;
  return true;
}

TransportFrames::Admission TransportFrames::Admit(const FrameIdentityMeta& frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!capacity_ || !peer_ || !frame.channel || !*frame.channel || std::strlen(frame.channel) > 64 ||
      !frame.operation || !frame.sequence || frame.operation < operation_ || frame.sequence <= sequence_ ||
      !GST_CLOCK_TIME_IS_VALID(frame.presentation_pts) ||
      (GST_CLOCK_TIME_IS_VALID(last_pts_) && frame.presentation_pts <= last_pts_) ||
      (frame.operation == operation_ && frame.channel != channel_))
    return {};
  if (frame.operation != operation_) {
    frames_.clear();
    operation_ = frame.operation;
    channel_ = frame.channel;
    need_keyframe_ = true;
  }
  if (frames_.size() >= capacity_) {
    // Losing any pending reference picture invalidates later deltas. Retire
    // the whole outstanding set so an older IDR cannot subsequently clear
    // recovery even though a newer reference picture was discarded.
    frames_.clear();
    need_keyframe_ = true;
  }
  frames_.push_back({frame.channel, frame.operation, frame.sequence, peer_, frame.source_pts, frame.presentation_pts});
  sequence_ = frame.sequence;
  last_pts_ = frame.presentation_pts;
  return {true, need_keyframe_};
}

std::optional<TransportFrame> TransportFrames::Encoded(GstClockTime pts, bool keyframe) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto frame =
      std::find_if(frames_.begin(), frames_.end(), [pts](const auto& value) { return value.presentation_pts == pts; });
  if (frame == frames_.end() || frame->encoded)
    return std::nullopt;
  if (need_keyframe_ && !keyframe) {
    frames_.erase(frame);
    return std::nullopt;
  }
  frame->encoded = true;
  frame->keyframe = keyframe;
  return *frame;
}

TransportFrames::Packet TransportFrames::Rtp(
    GstClockTime pts,
    std::uint32_t timestamp,
    std::uint32_t ssrc,
    bool marker) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto frame =
      std::find_if(frames_.begin(), frames_.end(), [pts](const auto& value) { return value.presentation_pts == pts; });
  if (frame == frames_.end() || !frame->encoded)
    return {};
  // Encoded AUs are ordered, but a caller may lose an earlier AU before this
  // packet boundary. That picture may be a reference even though its encoder
  // notification succeeded. Require an IDR across that missing packet gap too.
  if (std::any_of(frames_.begin(), frame, [](const auto& earlier) { return earlier.encoded; }))
    need_keyframe_ = true;
  if (need_keyframe_ && !frame->keyframe) {
    frames_.clear();
    return {};
  }
  if (!marker)
    return {true, std::nullopt};
  // Encoding is not delivery to the RTP boundary. Raw admission can run ahead
  // and retire this access unit; only its surviving final packet establishes
  // the reference picture on which later deltas may depend.
  if (frame->keyframe)
    need_keyframe_ = false;
  PacketFrame result{*frame, timestamp, ssrc};
  // One hardware encoder emits access units in order (B frames disabled).
  // Retire missing earlier frames as well, keeping metadata strictly bounded.
  frames_.erase(frames_.begin(), std::next(frame));
  return {true, std::move(result)};
}

std::size_t TransportFrames::pending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frames_.size();
}
} // namespace hm::preview
