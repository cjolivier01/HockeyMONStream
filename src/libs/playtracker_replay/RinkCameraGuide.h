#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

#include "hockeymon/csrc/play_tracker/BoxUtils.h"
#include "hstream/src/libs/stitching/PlaybackRinkMap.h"

namespace hm::playtracker_replay {

struct RinkCameraSettings {
  bool anticipate{true};
  bool frame_goals{false};
  bool adaptive_zoom{false};
};

struct RinkCameraSetup {
  std::shared_ptr<const stitching::PlaybackRinkMap> map;
  RinkCameraSettings settings;
};

struct RinkCameraInfo {
  unsigned located{0};
  double speed_mps{0};
  double lead_pixels{0};
  double zoom_ratio{1};
  int goal{0}; // -1 left, +1 right; zero means no goal framing request.
  bool active{false};
};

// Experimental framing adjustment over the native camera. Inputs have already
// passed the existing mask and native policy. This never changes detections,
// tracker state, or player exclusions. All work uses CPU metadata only.
class RinkCameraGuide {
 public:
  struct Track {
    uint64_t id;
    hm::BBox box;
    bool ignored{false};
  };
  RinkCameraGuide(RinkCameraSetup setup, double rotation, unsigned width, unsigned height);
  hm::BBox Step(
      uint64_t pts_ns,
      const hm::BBox& baseline,
      const hm::BBox& arena,
      const std::optional<hm::BBox>& cluster,
      const Track* tracks,
      size_t count,
      RinkCameraInfo* info);

 private:
  struct History {
    uint64_t id{0}, reference_ns{0}, seen_ns{0};
    cv::Point2d point{}, pixel{}, velocity{}, pixel_velocity{};
    bool used{false}, moving_sample{false};
  };
  RinkCameraSetup setup_;
  double rotation_;
  unsigned width_, height_;
  std::array<std::optional<cv::Point2d>, 2> goals_;
  std::array<History, 512> history_{};
  std::optional<uint64_t> previous_ns_;
  hm::BBox previous_box_;
  RinkCameraInfo previous_info_;
  cv::Point2d offset_{};
  double scale_{1};
};

} // namespace hm::playtracker_replay
