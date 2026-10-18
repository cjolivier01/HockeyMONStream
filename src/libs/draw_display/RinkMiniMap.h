#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>

#include <nvdsmeta.h>

#include "hstream/src/libs/draw_display/AnalyticsOverlay.h"
#include "hstream/src/libs/draw_display/RinkMiniMapOptions.h"
#include "hstream/src/libs/draw_display/RinkTrackSmoothing.h"
#include "hstream/src/libs/stitching/PlaybackRinkMap.h"

namespace hm::draw_display {

// Preview-only drawing into the existing GPU framebuffer. The prepared map
// is immutable; each renderer owns its generation cache. No file/video I/O.
class RinkMiniMap {
 public:
  explicit RinkMiniMap(std::shared_ptr<const stitching::PlaybackRinkMap> map, RinkMiniMapOptions options = {});
  bool Append(
      const NvDsFrameMeta* frame,
      unsigned stitched_width,
      unsigned stitched_height,
      float display_width,
      float display_height,
      analytics::CommandList* commands);

 private:
  std::shared_ptr<const stitching::PlaybackRinkMap> map_;
  RinkMiniMapOptions options_;
  std::vector<std::vector<cv::Point2d>> layout_lines_;
  std::array<cv::Point2d, 33> centre_circle_;
  std::string output_generation_;
  unsigned width_{0}, height_{0};
  std::optional<double> rotation_;
  RinkTrackSmoothing smoothing_;
  std::optional<uint64_t> last_pts_, analytics_epoch_;
  int last_frame_num_{0};
  unsigned source_id_{0};
};

} // namespace hm::draw_display
