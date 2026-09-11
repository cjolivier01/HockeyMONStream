#include "hstream/src/apps/apps-common/EncoderDimensions.h"

#include <unistd.h>

#include <atomic>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct TestFile {
  gchar* path{nullptr};
  TestFile() {
    const int fd = g_file_open_tmp("hstream-encoder-XXXXXX", &path, nullptr);
    if (fd >= 0)
      close(fd);
  }
  ~TestFile() {
    if (path)
      unlink(path);
    g_free(path);
  }
};

bool contains_red_frame(const char* encoded_path) {
  // Test-only CPU decode: unsupported NVIDIA transforms can still emit nonempty
  // encoded buffers. Inspect a 32x32 decoded image to reject blank/corrupt output.
  TestFile decoded;
  if (!decoded.path)
    return false;
  std::vector<std::string> arguments = {
      "ffmpeg",
      "-v",
      "error",
      "-threads",
      "1",
      "-i",
      encoded_path,
      "-vf",
      "scale=32:32",
      "-frames:v",
      "1",
      "-pix_fmt",
      "rgb24",
      "-f",
      "rawvideo",
      "-y",
      decoded.path};
  std::vector<gchar*> argv;
  for (auto& argument : arguments)
    argv.push_back(argument.data());
  argv.push_back(nullptr);
  gint status = -1;
  if (!g_spawn_sync(
          nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, nullptr, &status, nullptr) ||
      status != 0)
    return false;
  gchar* pixels = nullptr;
  gsize length = 0;
  if (!g_file_get_contents(decoded.path, &pixels, &length, nullptr))
    return false;
  bool ok = length == 32 * 32 * 3;
  for (gsize i = 0; ok && i < length; i += 3) {
    const auto* rgb = reinterpret_cast<const unsigned char*>(pixels + i);
    ok = rgb[0] > 180 && rgb[1] < 70 && rgb[2] < 70;
  }
  g_free(pixels);
  if (!ok)
    std::cerr << "Encoded output did not preserve the red test frame\n";
  return ok;
}

} // namespace

// Manual hardware check: the archive receives a downscaled NVMM frame while a
// sibling branch must still receive the original 10000x3000 stitched canvas.
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  const bool hevc = argc < 2 || std::string(argv[1]) != "h264";
  const bool main10 = argc >= 2 && std::string(argv[1]) == "main10";
  GError* error = nullptr;
  const std::string encoder_name = hevc ? "nvv4l2h265enc" : "nvv4l2h264enc";
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  const std::string converter_properties = main10 ? "compute-hw=2 copy-hw=2" : "compute-hw=1 copy-hw=2";
#else
  const std::string converter_properties = "compute-hw=1";
#endif
  const std::string description =
      "videotestsrc num-buffers=3 pattern=red ! video/x-raw,format=I420,width=10000,height=3000,framerate=1/1 "
      "! nvvideoconvert " +
      converter_properties + " ! video/x-raw(memory:NVMM),format=" + std::string(main10 ? "BGR10A2_LE" : "RGBA") +
      " ! tee name=full "
      "full. ! queue ! fakesink name=upstream sync=false "
      "full. ! queue ! nvvideoconvert " +
      converter_properties +
      " name=resize ! "
      "capsfilter name=output caps=video/x-raw(memory:NVMM),format=" +
      std::string(main10 ? "P010_10LE" : "I420") + " ! " + encoder_name +
      " name=encoder ! filesink name=archive sync=false";
  GstElement* pipeline = gst_parse_launch(description.c_str(), &error);
  if (error || !pipeline) {
    std::cerr << "Pipeline creation failed: " << (error ? error->message : "unknown") << '\n';
    return 1;
  }
  GstElement* encoder = gst_bin_get_by_name(GST_BIN(pipeline), "encoder");
  GstElement* resize = gst_bin_get_by_name(GST_BIN(pipeline), "resize");
  GstElement* output = gst_bin_get_by_name(GST_BIN(pipeline), "output");
  GstElement* upstream = gst_bin_get_by_name(GST_BIN(pipeline), "upstream");
  GstElement* archive = gst_bin_get_by_name(GST_BIN(pipeline), "archive");
  TestFile encoded_file;
  if (!encoded_file.path)
    return 1;
  g_object_set(archive, "location", encoded_file.path, nullptr);
  auto limits = hm::query_encoder_dimensions(encoder, hevc, 0);
  if (!limits || !hm::install_encoder_dimension_limit(resize, output, *limits, main10)) {
    std::cerr << "Could not query/install encoder limits\n";
    return 1;
  }
  std::cout << "Hardware range " << limits->min_width << 'x' << limits->min_height << " to " << limits->max_width << 'x'
            << limits->max_height << '\n';
  std::atomic<guint> encoded_buffers{0};
  GstPad* encoded = gst_element_get_static_pad(encoder, "src");
  const gulong counter = gst_pad_add_probe(
      encoded,
      GST_PAD_PROBE_TYPE_BUFFER,
      [](GstPad*, GstPadProbeInfo* info, gpointer data) {
        if (gst_buffer_get_size(GST_PAD_PROBE_INFO_BUFFER(info)) > 0)
          ++*static_cast<std::atomic<guint>*>(data);
        return GST_PAD_PROBE_OK;
      },
      &encoded_buffers,
      nullptr);
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
  if (encoded_buffers != 3) {
    std::cerr << "Expected 3 nonempty encoded frames, received " << encoded_buffers << '\n';
    ok = false;
  }
  std::cout << "Encoded frames: " << encoded_buffers << '\n';
  ok = contains_red_frame(encoded_file.path) && ok;
  gst_pad_remove_probe(encoded, counter);
  gst_object_unref(encoded);
  if (message)
    gst_message_unref(message);
  for (auto* element : {encoder, resize, output, upstream, archive})
    gst_object_unref(element);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  return ok ? 0 : 1;
}
