#pragma once

namespace hm::media {
// Reflections in the original decoded image axes. Applying both is a half turn.
struct FrameFlip {
  bool horizontal{};
  bool vertical{};

  bool enabled() const {
    return horizontal || vertical;
  }
  bool operator==(const FrameFlip& other) const {
    return horizontal == other.horizontal && vertical == other.vertical;
  }
  int nvFlipMethod() const {
    if (horizontal && vertical)
      return 2;
    if (horizontal)
      return 4;
    return vertical ? 6 : 0;
  }
};
} // namespace hm::media
