#include "PreviewTransportMonitor.h"

#include <gst/rtp/gstrtpbuffer.h>

#include <stdexcept>
#include <utility>

namespace hm::preview {
TransportMonitor::TransportMonitor(std::size_t frames, std::size_t events) : frames_(frames), event_capacity_(events) {
  if (!frames || !events)
    throw std::invalid_argument("Preview transport requires explicit nonzero frame/event capacities");
}

bool TransportMonitor::SetPeer(std::uint64_t peer) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!frames_.SetPeer(peer))
    return false;
  events_.clear();
  return true;
}

void TransportMonitor::Recover() {
  std::lock_guard<std::mutex> lock(mutex_);
  frames_.DiscardPending();
  events_.clear();
}

bool TransportMonitor::Select(const std::string& channel, std::uint64_t operation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!operation || operation < operation_ || channel.size() > 64)
    return false;
  frames_.DiscardPending();
  events_.clear();
  channel_ = channel;
  operation_ = operation;
  return true;
}

TransportFrames::Admission TransportMonitor::Admit(const FrameIdentityMeta* identity) {
  std::lock_guard<std::mutex> lock(mutex_);
  TransportFrames::Admission result;
  if (identity && identity->channel && !channel_.empty() && identity->channel == channel_ &&
      identity->operation == operation_)
    result = frames_.Admit(*identity);
  if (result.accepted)
    ++counters_.admitted;
  else
    ++counters_.rejected_raw;
  return result;
}

void TransportMonitor::KeyframeRequest(bool accepted) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++counters_.keyframe_requests;
  if (!accepted)
    ++counters_.keyframe_failures;
}

void TransportMonitor::ProbeFailure() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  frames_.DiscardPending();
  ++counters_.probe_failures;
}

void TransportMonitor::Push(TransportEvent event) {
  if (events_.size() == event_capacity_) {
    events_.pop_front();
    ++counters_.events_dropped;
  }
  events_.push_back(std::move(event));
}

bool TransportMonitor::Encoded(GstBuffer* buffer) {
  std::lock_guard<std::mutex> lock(mutex_);
  GstMapInfo map{};
  std::optional<TransportFrame> frame;
  if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    const bool idr = H264AccessUnitHasIdr(map.data, map.size);
    gst_buffer_unmap(buffer, &map);
    frame = frames_.Encoded(GST_BUFFER_PTS(buffer), idr);
  } else {
    // Unknown compressed output may be a decoder reference; no later delta
    // can be trusted until a newly requested IDR has crossed the RTP boundary.
    frames_.DiscardPending();
  }
  if (!frame) {
    ++counters_.rejected_encoded;
    return false;
  }
  ++counters_.encoded;
  Push({TransportEvent::Kind::kEncoded, std::move(*frame)});
  return true;
}

bool TransportMonitor::Rtp(GstBuffer* buffer) {
  std::lock_guard<std::mutex> lock(mutex_);
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  TransportFrames::Packet packet;
  if (buffer && gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp)) {
    packet = frames_.Rtp(
        GST_BUFFER_PTS(buffer),
        gst_rtp_buffer_get_timestamp(&rtp),
        gst_rtp_buffer_get_ssrc(&rtp),
        gst_rtp_buffer_get_marker(&rtp));
    gst_rtp_buffer_unmap(&rtp);
  } else {
    frames_.DiscardPending();
  }
  if (!packet.accepted) {
    ++counters_.rejected_rtp;
    return false;
  }
  ++counters_.packets;
  if (packet.completed) {
    ++counters_.packetized;
    Push({
        TransportEvent::Kind::kPacketized,
        *packet.completed,
        packet.completed->rtp_timestamp,
        packet.completed->ssrc,
    });
  }
  return true;
}

bool TransportMonitor::FilterRtp(GstBufferList* list) {
  if (!list || !gst_buffer_list_is_writable(list)) {
    ProbeFailure();
    return false;
  }
  for (guint index = 0; index < gst_buffer_list_length(list);) {
    if (Rtp(gst_buffer_list_get(list, index)))
      ++index;
    else
      gst_buffer_list_remove(list, index, 1);
  }
  return gst_buffer_list_length(list) != 0;
}

std::vector<TransportEvent> TransportMonitor::TakeEvents() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<TransportEvent> result;
  result.reserve(events_.size());
  while (!events_.empty()) {
    result.push_back(std::move(events_.front()));
    events_.pop_front();
  }
  return result;
}

TransportCounters TransportMonitor::counters() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return counters_;
}
} // namespace hm::preview
