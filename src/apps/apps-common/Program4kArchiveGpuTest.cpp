#include "hstream/src/apps/apps-common/deepstream_sinks.h"

#include <gst/gst.h>
#include <nvbufsurftransform.h>
#include <unistd.h>

#include <iostream>

GST_DEBUG_CATEGORY(NVDS_APP);

// Exercise the real sink factory: a full-resolution Program archive and its
// optional upload copy must negotiate different sizes without CPU readback.
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);
  bool ok = hm::sink_type_from_string("ENCODE_PROGRAM_4K_FILE") == NV_DS_SINK_ENCODE_PROGRAM_4K_FILE &&
      hm::to_string(NV_DS_SINK_ENCODE_PROGRAM_4K_FILE) == "ENCODE_PROGRAM_4K_FILE" &&
      hm::interpolation_method_from_string("default") == NvBufSurfTransformInter_Default &&
      hm::interpolation_method_from_string("nearest") == NvBufSurfTransformInter_Nearest &&
      hm::interpolation_method_from_string("bilinear") == NvBufSurfTransformInter_Bilinear &&
      hm::interpolation_method_from_string("algo1") == NvBufSurfTransformInter_Algo1 &&
      hm::interpolation_method_from_string("cubic") == NvBufSurfTransformInter_Algo1 &&
      hm::interpolation_method_from_string("bicubic") == NvBufSurfTransformInter_Algo1 &&
      hm::interpolation_method_from_string("algo2") == NvBufSurfTransformInter_Algo2 &&
      hm::interpolation_method_from_string("super") == NvBufSurfTransformInter_Algo2 &&
      hm::interpolation_method_from_string("algo3") == NvBufSurfTransformInter_Algo3 &&
      hm::interpolation_method_from_string("lanczos") == NvBufSurfTransformInter_Algo3 &&
      hm::interpolation_method_from_string("algo4") == NvBufSurfTransformInter_Algo4 &&
      hm::interpolation_method_from_string("nicest") == NvBufSurfTransformInter_Algo4 &&
      !hm::interpolation_method_from_string("bogus").has_value();
  for (const auto& size : {std::make_pair(4096, 2304), std::make_pair(4096, 2048), std::make_pair(1920, 1080)}) {
    const bool override_with_cubic = size == std::make_pair(4096, 2304);
    gchar* paths[2]{};
    NvDsSinkSubBinConfig configs[2]{};
    for (int i = 0; i < 2; ++i) {
      const int fd = g_file_open_tmp("hstream-program-4k-XXXXXX.mkv", &paths[i], nullptr);
      if (fd < 0)
        return 1;
      close(fd);
      configs[i].enable = TRUE;
      configs[i].sink_id = i;
      configs[i].type = i == 0 ? NV_DS_SINK_ENCODE_FILE : NV_DS_SINK_ENCODE_PROGRAM_4K_FILE;
      configs[i].encoder_config.codec = NV_DS_ENCODER_H265;
      configs[i].encoder_config.container = NV_DS_CONTAINER_MKV;
      configs[i].encoder_config.bitrate = 45000000;
      configs[i].encoder_config.output_file_path = paths[i];
      if (i == 1 && override_with_cubic) {
        configs[i].encoder_config.interpolation_method = NvBufSurfTransformInter_Algo1;
        configs[i].encoder_config.interpolation_method_set = TRUE;
      }
    }
    NvDsSinkBin sinks{};
    if (!create_sink_bin(2, configs, &sinks, 0))
      return 1;
    gint full_resolution_interpolation = NvBufSurfTransformInter_Nearest;
    gint upload_interpolation = NvBufSurfTransformInter_Default;
    gint upload_compute_hw = 0;
    g_object_get(G_OBJECT(sinks.sub_bins[0].transform), "interpolation-method", &full_resolution_interpolation, NULL);
    g_object_get(
        G_OBJECT(sinks.sub_bins[1].transform),
        "interpolation-method",
        &upload_interpolation,
        "compute-hw",
        &upload_compute_hw,
        NULL);
    ok &= full_resolution_interpolation == NvBufSurfTransformInter_Default &&
        upload_interpolation ==
            (override_with_cubic ? NvBufSurfTransformInter_Algo1 : NvBufSurfTransformInter_Default) &&
        upload_compute_hw == 1;
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
    const char* upload_properties = "compute-hw=1 copy-hw=2";
#else
    const char* upload_properties = "compute-hw=1";
#endif
    gchar* description = g_strdup_printf(
        "videotestsrc num-buffers=3 pattern=red ! video/x-raw,format=I420,width=%d,height=%d,framerate=1/1 "
        "! nvvideoconvert %s ! video/x-raw(memory:NVMM),format=RGBA ! identity name=program",
        size.first,
        size.second,
        upload_properties);
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description, &error);
    g_free(description);
    if (!pipeline || error)
      return 1;
    GstElement* program = gst_bin_get_by_name(GST_BIN(pipeline), "program");
    gst_bin_add(GST_BIN(pipeline), sinks.bin);
    if (!gst_element_link(program, sinks.bin))
      return 1;
    ok &= gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* message = gst_bus_timed_pop_filtered(
        bus, 45 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    ok &= message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      std::cerr << error->message << ": " << (debug ? debug : "") << '\n';
      g_clear_error(&error);
      g_free(debug);
    }
    const auto copy_size = size.first > 3840 ? std::make_pair(3840, size.second * 3840 / size.first) : size;
    for (int i = 0; i < 2; ++i) {
      GstPad* pad = gst_element_get_static_pad(sinks.sub_bins[i].encoder, "sink");
      GstCaps* caps = gst_pad_get_current_caps(pad);
      gint width = 0;
      gint height = 0;
      const auto expected = i == 0 ? size : copy_size;
      ok &= caps && gst_structure_get_int(gst_caps_get_structure(caps, 0), "width", &width) &&
          gst_structure_get_int(gst_caps_get_structure(caps, 0), "height", &height) && width == expected.first &&
          height == expected.second && gst_caps_features_contains(gst_caps_get_features(caps, 0), "memory:NVMM");
      std::cout << (i == 0 ? "Program: " : "4K copy: ") << width << 'x' << height << '\n';
      if (caps)
        gst_caps_unref(caps);
      gst_object_unref(pad);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    // Decode a bounded 32x32 sample in this hardware test to reject blank frames.
    for (const auto path : paths) {
      gchar* pixels = nullptr;
      gsize length = 0;
      gchar* sample_path = nullptr;
      const int fd = g_file_open_tmp("hstream-program-sample-XXXXXX", &sample_path, nullptr);
      if (fd < 0)
        return 1;
      close(fd);
      gchar* argv[] = {
          const_cast<gchar*>("ffmpeg"),
          const_cast<gchar*>("-v"),
          const_cast<gchar*>("error"),
          const_cast<gchar*>("-i"),
          path,
          const_cast<gchar*>("-vf"),
          const_cast<gchar*>("scale=32:32"),
          const_cast<gchar*>("-frames:v"),
          const_cast<gchar*>("1"),
          const_cast<gchar*>("-pix_fmt"),
          const_cast<gchar*>("rgb24"),
          const_cast<gchar*>("-f"),
          const_cast<gchar*>("rawvideo"),
          const_cast<gchar*>("-y"),
          sample_path,
          nullptr};
      gint status = -1;
      ok &= g_spawn_sync(
                nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, nullptr, &status, nullptr) &&
          status == 0 && g_file_get_contents(sample_path, &pixels, &length, nullptr) && length == 32 * 32 * 3;
      for (gsize i = 0; i + 2 < length; i += 3)
        ok &= static_cast<unsigned char>(pixels[i]) > 180 && static_cast<unsigned char>(pixels[i + 1]) < 70 &&
            static_cast<unsigned char>(pixels[i + 2]) < 70;
      g_free(pixels);
      unlink(sample_path);
      g_free(sample_path);
      unlink(path);
      g_free(path);
    }
    if (message)
      gst_message_unref(message);
    gst_object_unref(bus);
    gst_object_unref(program);
    gst_object_unref(pipeline);
  }
  return ok ? 0 : 1;
}
