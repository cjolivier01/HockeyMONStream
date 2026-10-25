#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>

#include "hstream/src/libs/player_analytics/Types.h"

typedef struct _NvDsFrameMeta NvDsFrameMeta;

#include "hstream/src/libs/draw_display/AnalyticsOverlay.h"
#include "hstream/src/libs/draw_display/RinkMiniMapOptions.h"
#include "hstream/src/libs/draw_display/RinkTrackSmoothing.h"
#include "hstream/src/libs/stitching/PlaybackRinkMap.h"

namespace hm::draw_display {

// Backend-neutral frame data in original stitched coordinates. No GPU pointers.
struct RinkMapPlayer {
  float left{}, top{}, width{}, height{};
  analytics::Color color;
  uint64_t track_id{player_analytics::kUntrackedId};
  bool ignored{false};
};
struct RinkMapFrame {
  std::string generation;
  unsigned stitched_width{}, stitched_height{};
  float coordinate_width{}, coordinate_height{};
  uint64_t pts{player_analytics::kInvalidTime}, sequence{};
  unsigned source_id{};
  bool have_tracks{false};
  std::vector<RinkMapPlayer> players;
  size_t total_players{};
  std::shared_ptr<const player_analytics::FrameResult> analytics;
};
// NVIDIA adapter; the drawing core does not depend on DeepStream.
RinkMapFrame ReadRinkMapFrame(const NvDsFrameMeta* frame, unsigned width, unsigned height);

// Shared preview and encoded-output drawing. The prepared map
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
  bool Append(const RinkMapFrame& frame, float width, float height, analytics::CommandList* commands);
  void Reset();
  const char* unavailableReason() const {
    return unavailable_reason_;
  }

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
  // Latched once this stream names a side. Teams do not become unnamed, so
  // a frame whose results are missing or all undecided must not drop the
  // overlay back to track colours for a frame.
  bool named_sides_{false};
  uint64_t last_frame_num_{0};
  unsigned source_id_{0};
  const char* unavailable_reason_{"Waiting for a preview frame."};
};

} // namespace hm::draw_display
