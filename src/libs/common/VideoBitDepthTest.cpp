#include "hstream/src/libs/common/pipeline_utils.h"

#include <iostream>

namespace {

bool expect_depth(const char* caps_text, unsigned int expected) {
  GstCaps* caps = gst_caps_from_string(caps_text);
  const std::optional<unsigned int> depth = hm::videoBitDepthFromCaps(caps);
  if (caps)
    gst_caps_unref(caps);
  if (depth.has_value() && *depth == expected)
    return true;
  std::cerr << "Expected depth " << expected << " for " << caps_text << ", got "
            << (depth.has_value() ? std::to_string(*depth) : "unknown") << '\n';
  return false;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  bool ok = true;
  ok &= expect_depth("video/x-h264, profile=(string)high, bit-depth-luma=(uint)8, bit-depth-chroma=(uint)8", 8);
  ok &= expect_depth("video/x-h265, profile=(string)main-10, bit-depth-luma=(uint)10", 10);
  ok &= expect_depth("video/x-raw, format=(string)P010_10LE", 10);
  ok &= expect_depth("video/x-av1, profile=(string)main-12", 12);
  ok &= expect_depth("video/x-h265, bit-depth-luma=(uint)8, bit-depth-chroma=(uint)10", 8);
  ok &= expect_depth(
      "video/x-h265, profile=(string)main, bit-depth-luma=(uint)8; "
      "video/x-h265, profile=(string)main-10, bit-depth-luma=(uint)10",
      8);

  GstCaps* unknown = gst_caps_from_string("video/x-h264, profile=(string)high");
  ok &= !hm::videoBitDepthFromCaps(unknown).has_value();
  gst_caps_unref(unknown);
  if (!ok)
    std::cerr << "Unknown caps must remain unknown instead of being treated as high-bit input\n";
  return ok ? 0 : 1;
}
