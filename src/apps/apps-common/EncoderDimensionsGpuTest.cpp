#include "hstream/src/apps/apps-common/EncoderDimensions.h"

#include <iostream>
#include <string>

// Manual hardware check: the archive receives a downscaled NVMM frame while a
// sibling branch must still receive the original 10000x3000 stitched canvas.
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  const bool hevc = argc < 2 || std::string(argv[1]) != "h264";
  const bool main10 = argc >= 2 && std::string(argv[1]) == "main10";
  GError* error = nullptr;
  const std::string encoder_name = hevc ? "nvv4l2h265enc" : "nvv4l2h264enc";
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  const std::string converter_properties = "compute-hw=1 copy-hw=2";
#else
  const std::string converter_properties = "compute-hw=1";
#endif
  const std::string description =
      "videotestsrc num-buffers=3 ! video/x-raw,format=I420,width=10000,height=3000,framerate=1/1 "
      "! nvvideoconvert " +
      converter_properties +
      " ! video/x-raw(memory:NVMM),format=I420 ! tee name=full "
      "full. ! queue ! fakesink name=upstream sync=false "
      "full. ! queue ! nvvideoconvert " +
      converter_properties +
      " name=resize ! "
      "capsfilter name=output caps=video/x-raw(memory:NVMM),format=" +
      std::string(main10 ? "P010_10LE" : "I420") + " ! " + encoder_name + " name=encoder ! fakesink sync=false";
  GstElement* pipeline = gst_parse_launch(description.c_str(), &error);
  if (error || !pipeline) {
    std::cerr << "Pipeline creation failed: " << (error ? error->message : "unknown") << '\n';
    return 1;
  }
  GstElement* encoder = gst_bin_get_by_name(GST_BIN(pipeline), "encoder");
  GstElement* resize = gst_bin_get_by_name(GST_BIN(pipeline), "resize");
  GstElement* output = gst_bin_get_by_name(GST_BIN(pipeline), "output");
  GstElement* upstream = gst_bin_get_by_name(GST_BIN(pipeline), "upstream");
  auto limits = hm::query_encoder_dimensions(encoder, hevc, 0);
  if (!limits || !hm::install_encoder_dimension_limit(resize, output, *limits)) {
    std::cerr << "Could not query/install encoder limits\n";
    return 1;
  }
  std::cout << "Hardware range " << limits->min_width << 'x' << limits->min_height << " to " << limits->max_width << 'x'
            << limits->max_height << '\n';
  bool ok = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
  GstBus* bus = gst_element_get_bus(pipeline);
  GstMessage* message = gst_bus_timed_pop_filtered(
      bus, 45 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  if (!message || GST_MESSAGE_TYPE(message) != GST_MESSAGE_EOS) {
    ok = false;
    if (message) {
      gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      std::cerr << error->message << ": " << (debug ? debug : "") << '\n';
      g_clear_error(&error);
      g_free(debug);
    } else {
      std::cerr << "Timed out waiting for encoded frames\n";
    }
  }
  const auto expected = hm::fit_encoder_dimensions(10000, 3000, *limits);
  for (auto entry : {std::make_pair(upstream, std::make_pair(10000U, 3000U)), std::make_pair(encoder, *expected)}) {
    GstPad* pad = gst_element_get_static_pad(entry.first, "sink");
    GstCaps* caps = gst_pad_get_current_caps(pad);
    gint width = 0;
    gint height = 0;
    if (!caps || !gst_structure_get_int(gst_caps_get_structure(caps, 0), "width", &width) ||
        !gst_structure_get_int(gst_caps_get_structure(caps, 0), "height", &height) ||
        width != static_cast<gint>(entry.second.first) || height != static_cast<gint>(entry.second.second) ||
        !gst_caps_features_contains(gst_caps_get_features(caps, 0), "memory:NVMM"))
      ok = false;
    std::cout << GST_ELEMENT_NAME(entry.first) << ": " << width << 'x' << height << '\n';
    if (caps)
      gst_caps_unref(caps);
    gst_object_unref(pad);
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
  if (message)
    gst_message_unref(message);
  for (auto* element : {encoder, resize, output, upstream})
    gst_object_unref(element);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  return ok ? 0 : 1;
}
