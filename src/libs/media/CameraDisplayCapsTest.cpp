#include "hstream/src/libs/media/CameraDisplayCaps.h"

#include <iostream>

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  auto* portrait = gst_caps_from_string(
      "video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=NV12,width=288,height=512,pixel-aspect-ratio=2/1,chroma-site=mpeg2");
  auto* landscape = gst_caps_from_string(
      "video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=NV12,width=512,height=288,pixel-aspect-ratio=1/2,chroma-site=mpeg2");
  auto* displayed = hm::media::CameraDisplayCaps(portrait, true);
  auto* round_trip = hm::media::CameraDisplayCaps(displayed, true);
  bool compatible = gst_caps_is_equal(displayed, landscape) && gst_caps_is_equal(round_trip, portrait);
  gst_caps_set_simple(landscape, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, nullptr);
  compatible &= !gst_caps_is_equal(displayed, landscape);
  gst_caps_set_simple(
      landscape, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 2, "chroma-site", G_TYPE_STRING, "jpeg", nullptr);
  compatible &= !gst_caps_is_equal(displayed, landscape);
  gst_caps_unref(portrait);
  gst_caps_unref(landscape);
  gst_caps_unref(displayed);
  gst_caps_unref(round_trip);
  if (!compatible)
    std::cerr << "Quarter-turned chapters must preserve displayed geometry, pixel aspect and chroma site\n";
  return compatible ? 0 : 1;
}
