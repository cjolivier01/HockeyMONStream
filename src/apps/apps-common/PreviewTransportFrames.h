#pragma once

#include "hstream/src/apps/apps-common/SelectedPreviewStream.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace hm::preview {

// The parser must negotiate byte-stream/alignment=au. GstBuffer's non-delta
// flag alone also admits non-IDR I pictures, which cannot start a new peer GOP.
bool H264AccessUnitHasIdr(const std::uint8_t* bytes, std::size_t size);

struct TransportFrame {
  std::string channel;
  std::uint64_t operation, sequence, peer;
  GstClockTime source_pts, presentation_pts;
  bool encoded{false};
  bool keyframe{false};
};
struct PacketFrame : TransportFrame {
  std::uint32_t rtp_timestamp, ssrc;
};

// Bounded identity bookkeeping for a single selected stream/encoder/payloader.
// Only compressed packets may be inspected for their RTP header; raw pixels are
// never read. Overflow retires all outstanding access units and requires a new
// IDR so no later delta can depend on a dropped reference. Presentation remains
// a separate browser acknowledgement.
class TransportFrames {
 public:
  explicit TransportFrames(std::size_t capacity);
  // The signaling owner must set its independently increasing peer generation
  // before replacing transport. Pending old-peer encoded/RTP output is rejected.
  bool SetPeer(std::uint64_t peer);
  struct Admission {
    bool accepted{false};
    bool force_keyframe{false};
  };
  Admission Admit(const FrameIdentityMeta& frame);
  // Drops stale output and delta frames until a new route/peer's first IDR.
  std::optional<TransportFrame> Encoded(GstClockTime pts, bool keyframe);
  struct Packet {
    bool accepted{false};
    std::optional<PacketFrame> completed;
  };
  Packet Rtp(GstClockTime pts, std::uint32_t timestamp, std::uint32_t ssrc, bool marker);
  std::size_t pending() const;

 private:
  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::deque<TransportFrame> frames_;
  std::uint64_t peer_{0}, operation_{0}, sequence_{0};
  std::string channel_;
  GstClockTime last_pts_{GST_CLOCK_TIME_NONE};
  bool need_keyframe_{true};
};

} // namespace hm::preview
