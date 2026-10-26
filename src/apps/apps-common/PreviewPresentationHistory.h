#pragma once

#include "hstream/src/apps/apps-common/PreviewTransportFrames.h"

namespace hm::preview {

// Separate from the pending RTP ledger: a marker retires encoder bookkeeping,
// but a browser can report that frame much later. The monitor serializes calls.
// Receipt acceptance validates identity; it is still a client's observation,
// not independent proof of visible pixels.
class PresentationHistory {
 public:
  explicit PresentationHistory(std::size_t capacity);
  void Clear();
  void Record(const PacketFrame& frame);
  std::optional<PacketFrame> Presented(const PacketFrame& receipt);
  std::size_t size() const {
    return frames_.size();
  }
  std::uint64_t evicted() const {
    return evicted_;
  }

 private:
  const std::size_t capacity_;
  std::deque<PacketFrame> frames_;
  std::uint64_t presented_{0}, evicted_{0};
};

} // namespace hm::preview
