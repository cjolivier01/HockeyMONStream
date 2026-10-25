#include "PreviewPresentationHistory.h"

#include <algorithm>
#include <stdexcept>

namespace hm::preview {
PresentationHistory::PresentationHistory(std::size_t capacity) : capacity_(capacity) {
  if (!capacity || capacity > 65536)
    throw std::invalid_argument("Preview presentation history requires a capacity between 1 and 65536");
}

void PresentationHistory::Clear() {
  frames_.clear();
  // Sequence numbers increase for the target lifetime, including recovery.
}

void PresentationHistory::Record(const PacketFrame& frame) {
  if (frames_.size() == capacity_) {
    frames_.pop_front();
    ++evicted_;
  }
  frames_.push_back(frame);
}

std::optional<PacketFrame> PresentationHistory::Presented(const PacketFrame& receipt) {
  if (!receipt.sequence || receipt.sequence <= presented_)
    return {};
  const auto found = std::find_if(frames_.begin(), frames_.end(), [&](const auto& frame) {
    return frame.peer == receipt.peer && frame.operation == receipt.operation && frame.sequence == receipt.sequence &&
        frame.channel == receipt.channel && frame.rtp_timestamp == receipt.rtp_timestamp && frame.ssrc == receipt.ssrc;
  });
  if (found == frames_.end())
    return {};
  auto result = *found;
  presented_ = result.sequence;
  frames_.erase(frames_.begin(), std::next(found));
  return result;
}
} // namespace hm::preview
