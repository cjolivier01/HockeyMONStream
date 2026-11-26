#pragma once

#include <gst/gst.h>

namespace hm::media {
// Return an owned reference to fixed camera caps in the requested coordinates.
// A quarter turn exchanges both pixel axes and the sample aspect ratio.
inline GstCaps* CameraDisplayCaps(GstCaps* caps, bool transpose) {
  if (!transpose)
    return gst_caps_ref(caps);
  auto* result = gst_caps_copy(caps);
  auto* structure = gst_caps_get_structure(result, 0);
  int width, height, numerator, denominator;
  if (gst_structure_get_int(structure, "width", &width) && gst_structure_get_int(structure, "height", &height))
    gst_structure_set(structure, "width", G_TYPE_INT, height, "height", G_TYPE_INT, width, nullptr);
  if (gst_structure_get_fraction(structure, "pixel-aspect-ratio", &numerator, &denominator))
    gst_structure_set(structure, "pixel-aspect-ratio", GST_TYPE_FRACTION, denominator, numerator, nullptr);
  return result;
}
} // namespace hm::media
