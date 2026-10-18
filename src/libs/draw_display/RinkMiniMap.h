#pragma once

#include <memory>
#include <optional>
#include <string>

#include <nvdsmeta.h>

#include "hstream/src/libs/draw_display/AnalyticsOverlay.h"
#include "hstream/src/libs/stitching/PlaybackRinkMap.h"

namespace hm::draw_display {

// Preview-only drawing into the existing GPU framebuffer. The prepared map
// is immutable; each renderer owns its generation cache. No file/video I/O.
class RinkMiniMap {
 public:
  explicit RinkMiniMap(std::shared_ptr<const stitching::PlaybackRinkMap> map) : map_(std::move(map)) {}
  bool Append(
      const NvDsFrameMeta* frame,
      unsigned stitched_width,
      unsigned stitched_height,
      float display_width,
      float display_height,
      analytics::CommandList* commands);

 private:
  std::shared_ptr<const stitching::PlaybackRinkMap> map_;
  std::string output_generation_;
  unsigned width_{0}, height_{0};
  std::optional<double> rotation_;
};

} // namespace hm::draw_display
