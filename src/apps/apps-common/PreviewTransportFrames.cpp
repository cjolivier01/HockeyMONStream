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
  while (frames_.size() >= capacity_)
    frames_.pop_front();
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
  need_keyframe_ = false;
  frame->encoded = true;
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
  if (!marker)
    return {true, std::nullopt};
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
