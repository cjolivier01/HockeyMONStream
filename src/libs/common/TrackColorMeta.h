#pragma once

#include <gst/gst.h>
#include <nvdsmeta.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "hstream/src/libs/player_analytics/TrackColors.h"

namespace hm::preview_overlay {

// One instance belongs to the upstream snapshot producer, never to a renderer.
// Apply is called only when a player drawing consumer requested colors.
class TrackColorState {
 public:
  bool Apply(NvDsFrameMeta* frame) noexcept;
  void Reset() noexcept;

 private:
  struct Stream {
    player_analytics::TrackColorAllocator colors;
    std::string generation;
    uint64_t epoch{0};
    int64_t frame_num{0};
    uint32_t width{0};
    uint32_t height{0};
    bool initialized{false};
  };
  // The standard stitched graph has one source. A hard cap also bounds custom
  // multistream graphs without allowing unbounded source-id churn.
  static constexpr size_t kMaximumStreams = 64;
  std::unordered_map<uint32_t, Stream> streams_;
};

// Used only in graphs without vpplaytracker. Repeated configuration updates the
// same upstream owner, including the encoded-only drawing + preview combination.
bool ConfigureTrackColorProducer(
    GstElement* upstream,
    bool color_players,
    std::atomic<unsigned>* preview_flags = nullptr,
    const char* pad_name = "src") noexcept;

} // namespace hm::preview_overlay
