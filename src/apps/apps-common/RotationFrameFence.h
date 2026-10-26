#pragma once

#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace hm::preview {

// Kept separate from the NvDs adapter so exact crop/epoch matching can be
// tested without a renderer, decoder, encoder, or GPU readback.
struct RotationFrameObservation {
  bool epoch_present{false};
  std::string authorization_id;
  bool crop_present{false};
  float left{0.0F}, right{0.0F};
};

struct RotationFrameFence {
  std::string authorization_id;
  std::optional<std::pair<float, float>> angles;

  bool Matches(const std::string& channel, const RotationFrameObservation& frame) const {
    if (channel.compare(0, 6, "source") == 0)
      return true;
    if (channel != "program" && channel != "stitched")
      return false;
    if (!frame.epoch_present || frame.authorization_id != authorization_id)
      return false;
    if (channel == "stitched" || !angles)
      return true;
    return frame.crop_present && std::isfinite(frame.left) && std::isfinite(frame.right) &&
        frame.left == angles->first && frame.right == angles->second;
  }
};

} // namespace hm::preview
