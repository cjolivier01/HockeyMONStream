#pragma once

#include <gst/gst.h>
#include <iomanip>
#include <sstream>
#include <string>

namespace hm::pipeline {

// Bind the effective plugin policy, including native aliases and CLI overrides,
// rather than only the original, unadjusted rink-mask pixels.
inline std::string PlayerFrameScanMaskSettings(GstElement* element) {
  gfloat raise = 0, lower = 0, left = 0, right = 0;
  gint top_inset = 0, bottom_inset = 0, left_inset = 0, right_inset = 0;
  gboolean strict = FALSE;
  g_object_get(
      element,
      "raise-bbox-center-by-height-ratio",
      &raise,
      "lower-bbox-bottom-by-height-ratio",
      &lower,
      "left-bbox-by-half-width-ratio",
      &left,
      "right-bbox-by-half-width-ratio",
      &right,
      "mask-top-inset",
      &top_inset,
      "mask-bottom-inset",
      &bottom_inset,
      "mask-left-inset",
      &left_inset,
      "mask-right-inset",
      &right_inset,
      "require-existing-mask",
      &strict,
      nullptr);
  std::ostringstream result;
  result << std::setprecision(9) << "raise=" << raise << ";lower=" << lower << ";strict=" << strict << ";left=" << left
         << ";right=" << right << ";top_inset=" << top_inset << ";bottom_inset=" << bottom_inset
         << ";left_inset=" << left_inset << ";right_inset=" << right_inset;
  return result.str();
}

} // namespace hm::pipeline
