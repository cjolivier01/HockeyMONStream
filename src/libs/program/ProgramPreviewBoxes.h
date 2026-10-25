#pragma once

#include "hstream/src/libs/draw_display/AnalyticsOverlay.h"
#include "hstream/src/libs/draw_display/RinkMiniMap.h"
#include "hstream/src/libs/player_analytics/TrackColors.h"
#include "hstream/src/libs/program/FrameGeometry.h"
#include "hstream/src/libs/tracking/NativeTracker.h"

namespace hm::program {
// Metadata-only preview commands. Uses the shared bounded palette leases;
// Update once per observed frame, Build for each preview output geometry.
class ProgramPreviewBoxes {
 public:
  void Update(uint64_t pts_ns, const std::vector<tracking::Observation>& tracks);
  const draw_display::analytics::CommandList& Build(
      const playcropper::FrameTransformGeometry& transform,
      unsigned width,
      unsigned height);
  draw_display::RinkMapFrame RinkFrame(
      const std::string& generation,
      unsigned width,
      unsigned height,
      uint64_t pts,
      uint64_t sequence) const;
  void Reset();

 private:
  player_analytics::TrackColorAllocator colors_;
  std::vector<tracking::Observation> tracks_;
  draw_display::analytics::CommandList commands_;
};
} // namespace hm::program
