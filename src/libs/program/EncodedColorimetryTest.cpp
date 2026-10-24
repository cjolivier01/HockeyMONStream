#include "hstream/src/libs/program/EncodedColorimetry.h"
#include <gst/app/gstappsrc.h>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
// Exercise contract failures using compressed dummy data, with no video/GPU.
void Failure(const char* format, size_t bytes, size_t maximum) {
  GError* error{};
  auto* pipeline =
      gst_parse_launch("appsrc name=input format=time ! identity name=encoder ! fakesink sync=false", &error);
  Check(pipeline && !error, "Cannot create fixture");
  auto* input = gst_bin_get_by_name(GST_BIN(pipeline), "input");
  auto* encoder = gst_bin_get_by_name(GST_BIN(pipeline), "encoder");
  GstVideoInfo info{};
  Check(gst_video_info_set_format(&info, GST_VIDEO_FORMAT_NV12, 16, 16), "Video format");
  info.chroma_site = GST_VIDEO_CHROMA_SITE_JPEG;
  Check(gst_video_colorimetry_from_string(&info.colorimetry, "bt709"), "Color format");
  Check(
      !hm::program::InstallEncodedColorimetry(encoder, hm::program::EncodedCodec::kHevc, info, 0).ok(),
      "Missing bound accepted");
  auto unknown = info;
  unknown.colorimetry.range = GST_VIDEO_COLOR_RANGE_UNKNOWN;
  Check(
      !hm::program::InstallEncodedColorimetry(encoder, hm::program::EncodedCodec::kHevc, unknown, maximum).ok(),
      "Unknown range accepted");
  Check(
      hm::program::InstallEncodedColorimetry(encoder, hm::program::EncodedCodec::kHevc, info, maximum).ok(),
      "Filter setup failed");
  auto* caps = gst_caps_new_simple(
      "video/x-h265", "stream-format", G_TYPE_STRING, format, "alignment", G_TYPE_STRING, "au", nullptr);
  gst_app_src_set_caps(GST_APP_SRC(input), caps);
  gst_caps_unref(caps);
  Check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "Pipeline start");
  auto* buffer = gst_buffer_new_allocate(nullptr, bytes, nullptr);
  gst_buffer_memset(buffer, 0, 0, bytes);
  (void)gst_app_src_push_buffer(GST_APP_SRC(input), buffer);
  auto* bus = gst_element_get_bus(pipeline);
  auto* message = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND, GST_MESSAGE_ERROR);
  Check(message && GST_MESSAGE_SRC(message) == GST_OBJECT(encoder), "Missing encoder bus error");
  gst_message_unref(message);
  gst_object_unref(bus);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(encoder);
  gst_object_unref(input);
  gst_object_unref(pipeline);
}
} // namespace
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  try {
    Failure("hvc1", 16, 64); // Length-prefixed format is not accepted as Annex B.
    Failure("byte-stream", 65, 64); // Reject before any bitstream allocation/parse.
    Failure("byte-stream", 16, 64); // Invalid NAL units become a bus error.
    std::cout << "Encoded colorimetry rejects invalid caps, bounds, color and bitstreams\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
