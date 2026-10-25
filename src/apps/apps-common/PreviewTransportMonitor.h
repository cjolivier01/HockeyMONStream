#pragma once

#include "hstream/src/apps/apps-common/PreviewTransportFrames.h"

#include <deque>
#include <vector>

namespace hm::preview {

// Encoded/packetized are sender boundaries, never browser presentation. Events
// contain metadata only and are polled by their owner; streaming threads never
// invoke signaling callbacks or schedule an unbounded number of context tasks.
struct TransportEvent {
  enum class Kind { kEncoded, kPacketized };
  Kind kind;
  TransportFrame frame;
  std::uint32_t rtp_timestamp{0}, ssrc{0};
};
struct TransportCounters {
  std::uint64_t admitted{0}, encoded{0}, packets{0}, packetized{0};
  std::uint64_t rejected_raw{0}, rejected_encoded{0}, rejected_rtp{0};
  std::uint64_t keyframe_requests{0}, keyframe_failures{0}, events_dropped{0};
  std::uint64_t probe_failures{0};
};

class TransportMonitor {
 public:
  // Both capacities are mandatory; zero is invalid. No shipped defaults.
  TransportMonitor(std::size_t frame_capacity, std::size_t event_capacity);
  bool SetPeer(std::uint64_t peer);
  // Called after closing route gates, before opening a replacement. Empty
  // channel disables admission immediately, including delayed encoder output.
  bool Select(const std::string& channel, std::uint64_t operation);
  TransportFrames::Admission Admit(const FrameIdentityMeta* identity);
  void KeyframeRequest(bool accepted);
  // An exception after encoder admission may have dropped a reference picture.
  void ProbeFailure() noexcept;
  // Only compressed bytes/RTP headers are mapped. Raw GPU pixels never are.
  bool Encoded(GstBuffer* buffer);
  bool Rtp(GstBuffer* buffer);
  // Filters a caller-owned writable list packet by packet. Dropping a whole
  // mixed list after accepting its IDR marker would falsely end recovery.
  bool FilterRtp(GstBufferList* list);
  std::vector<TransportEvent> TakeEvents();
  TransportCounters counters() const;

 private:
  void Push(TransportEvent event);
  mutable std::mutex mutex_;
  TransportFrames frames_;
  const std::size_t event_capacity_;
  std::uint64_t operation_{0};
  std::string channel_;
  std::deque<TransportEvent> events_;
  TransportCounters counters_;
};

} // namespace hm::preview
