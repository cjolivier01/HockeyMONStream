#pragma once

#include "hstream/src/libs/media/FrameFlip.h"

namespace hm::media {
// An orthogonal MP4 display transform, expressed as transpose followed by
// reflections in the displayed axes. Translation only positions the result.
struct DisplayTransform {
  bool transpose{};
  FrameFlip flip{};

  DisplayTransform corrected(FrameFlip correction) const {
    return {transpose, {flip.horizontal != correction.horizontal, flip.vertical != correction.vertical}};
  }
  int nvFlipMethod() const {
    if (!transpose)
      return flip.nvFlipMethod();
    if (flip.horizontal)
      return flip.vertical ? 5 : 3;
    return flip.vertical ? 1 : 7;
  }
  // GstVideoOrientationMethod and GstGLRotateMethod use the same ordering.
  int gstOrientationMethod() const {
    constexpr int methods[]{0, 3, 2, 1, 4, 7, 5, 6};
    return methods[nvFlipMethod()];
  }
};
} // namespace hm::media
