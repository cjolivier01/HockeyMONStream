#include "hstream/src/apps/apps-common/ArchiveWatermark.h"

#include "hstream/src/libs/draw_display/AnalyticsOverlay.h"

#include <gst/gst.h>
#include <gst/video/video.h>
#include <nvbufsurface.h>

#include <chrono>
#include <iostream>
#include <string>

namespace {
struct Result {
  bool received{false};
  bool changed{false};
  bool chroma_changed{false};
  bool outside_unchanged{false};
  bool ten_bit{false};
};

#if defined(__aarch64__)
double BenchmarkArchive(bool watermark) {
  constexpr int kFrames = 240;
  GError* error = nullptr;
  GstElement* pipeline = gst_parse_launch(
      "videotestsrc pattern=black num-buffers=240 ! "
      "video/x-raw,format=I420,width=1920,height=1080,framerate=30/1 ! "
      "nvvideoconvert ! video/x-raw(memory:NVMM),format=P010_10LE,width=1920,height=1080 ! "
      "capsfilter name=archive_caps ! fakesink sync=false",
      &error);
  if (!pipeline || error) {
    if (error)
      g_error_free(error);
    if (pipeline)
      gst_object_unref(pipeline);
    return -1;
  }
  GstElement* caps_filter = gst_bin_get_by_name(GST_BIN(pipeline), "archive_caps");
  const bool installed = !watermark || (caps_filter && hm::archive_watermark::Install(caps_filter, 0));
  const auto start = std::chrono::steady_clock::now();
  if (installed)
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
  GstBus* bus = gst_element_get_bus(pipeline);
  GstMessage* message = installed
      ? gst_bus_timed_pop_filtered(
            bus, 30 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))
      : nullptr;
  const auto end = std::chrono::steady_clock::now();
  const bool complete = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
  if (message)
    gst_message_unref(message);
  gst_object_unref(bus);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  if (caps_filter)
    gst_object_unref(caps_filter);
  gst_object_unref(pipeline);
  return complete ? kFrames / std::chrono::duration<double>(end - start).count() : -1;
}

void OnGpuHandoff(GstElement*, GstBuffer* buffer, GstPad*, gpointer user_data) {
  auto* result = static_cast<Result*>(user_data);
  result->received = true;
  GstMapInfo map{};
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
    return;
  auto* surface = reinterpret_cast<NvBufSurface*>(map.data);
  if (map.size >= sizeof(NvBufSurface) && surface->surfaceList && surface->numFilled == 1 &&
      NvBufSurfaceMap(surface, 0, -1, NVBUF_MAP_READ) == 0) {
    NvBufSurfaceSyncForCpu(surface, 0, -1);
    const auto& params = surface->surfaceList[0];
    const auto* luma = static_cast<const uint8_t*>(params.mappedAddr.addr[0]);
    const size_t pitch = params.planeParams.pitch[0];
    const unsigned bytes_per_pixel = result->ten_bit ? 2U : 1U;
    const unsigned background = result->ten_bit ? *reinterpret_cast<const uint16_t*>(luma) : luma[0];
    result->outside_unchanged = background != 0;
    for (int y = 250; y < 360 && !result->changed; ++y)
      for (int x = 290; x < 608; ++x)
        if ((result->ten_bit ? *reinterpret_cast<const uint16_t*>(luma + y * pitch + x * bytes_per_pixel)
                             : luma[y * pitch + x]) > background + (result->ten_bit ? 64U : 1U)) {
          result->changed = true;
          break;
        }
    const auto* chroma = static_cast<const uint8_t*>(params.mappedAddr.addr[1]);
    const size_t chroma_pitch = params.planeParams.pitch[1];
    const unsigned chroma_background = result->ten_bit ? *reinterpret_cast<const uint16_t*>(chroma) : chroma[0];
    for (int y = 250; y < 360 && !result->chroma_changed; y += 2)
      for (int x = 290; x < 608; x += 2) {
        const unsigned sample = result->ten_bit
            ? *reinterpret_cast<const uint16_t*>(chroma + (y / 2) * chroma_pitch + x * 2)
            : chroma[(y / 2) * chroma_pitch + x / 2];
        if (sample != chroma_background) {
          result->chroma_changed = true;
          break;
        }
      }
    NvBufSurfaceUnMap(surface, 0, -1);
  }
  gst_buffer_unmap(buffer, &map);
}
#endif

void OnHandoff(GstElement*, GstBuffer* buffer, GstPad*, gpointer user_data) {
  auto* result = static_cast<Result*>(user_data);
  result->received = true;
  GstCaps* caps = gst_caps_from_string("video/x-raw,format=I420,width=640,height=360,framerate=30/1");
  GstVideoInfo info{};
  GstVideoFrame frame{};
  if (caps && gst_video_info_from_caps(&info, caps) && gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
    const auto* y_plane = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    result->outside_unchanged = y_plane[0] == 16;
    for (int y = 250; y < 360 && !result->changed; ++y)
      for (int x = 290; x < 608; ++x)
        if (y_plane[y * stride + x] > 20) {
          result->changed = true;
          break;
        }
    gst_video_frame_unmap(&frame);
  }
  if (caps)
    gst_caps_unref(caps);
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
#if defined(__aarch64__)
  if (argc == 2 && std::string(argv[1]) == "--benchmark") {
    const double baseline = BenchmarkArchive(false);
    const double marked = BenchmarkArchive(true);
    std::cout << "Jetson 1080p P010 archive frames/s baseline=" << baseline << " marked=" << marked << '\n';
    return baseline > 0 && marked > 0 ? 0 : 1;
  }
#endif
  GError* error = nullptr;
  GstElement* pipeline = gst_parse_launch(
      "videotestsrc pattern=black num-buffers=1 ! "
      "video/x-raw,format=I420,width=640,height=360,framerate=30/1 ! "
      "capsfilter name=archive_caps ! fakesink name=output signal-handoffs=true sync=false",
      &error);
  if (!pipeline || error) {
    std::cerr << "Could not create archive watermark fixture\n";
    if (error)
      g_error_free(error);
    if (pipeline)
      gst_object_unref(pipeline);
    return 1;
  }
  GstElement* caps_filter = gst_bin_get_by_name(GST_BIN(pipeline), "archive_caps");
  GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
  Result result;
  const bool installed = caps_filter && hm::archive_watermark::Install(caps_filter, 0);
  if (sink)
    g_signal_connect(sink, "handoff", G_CALLBACK(OnHandoff), &result);
  if (installed)
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
  GstBus* bus = gst_element_get_bus(pipeline);
  GstMessage* message = installed
      ? gst_bus_timed_pop_filtered(
            bus, 10 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))
      : nullptr;
  const bool ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS && result.received && result.changed &&
      result.outside_unchanged;
  if (!ok)
    std::cerr << "System-memory I420 archive watermark was missing or changed pixels outside its rectangle\n";
  if (message)
    gst_message_unref(message);
  gst_object_unref(bus);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  if (caps_filter)
    gst_object_unref(caps_filter);
  if (sink)
    gst_object_unref(sink);
  gst_object_unref(pipeline);
  if (!ok)
    return 1;

  // The mark is mandatory, so a font failure has to stop every frame, not just
  // the one that discovered it. The original bug cached a half-built atlas and
  // let the second buffer through unmarked, so push two.
  {
    error = nullptr;
    GstElement* font_pipeline = gst_parse_launch(
        "videotestsrc pattern=black num-buffers=2 ! "
        "video/x-raw,format=I420,width=640,height=360,framerate=30/1 ! "
        // async=false only removes a stall. Both buffers are pushed either
        // way - a dropped push still returns GST_FLOW_OK - but a prerolling
        // sink never sees one, so it never leaves ASYNC and never posts EOS,
        // and the drain below would wait out its full timeout.
        "capsfilter name=archive_caps ! fakesink name=output signal-handoffs=true sync=false async=false",
        &error);
    if (!font_pipeline || error) {
      std::cerr << "Could not create missing-font archive fixture\n";
      if (error)
        g_error_free(error);
      if (font_pipeline)
        gst_object_unref(font_pipeline);
      return 1;
    }
    GstElement* font_caps = gst_bin_get_by_name(GST_BIN(font_pipeline), "archive_caps");
    GstElement* font_sink = gst_bin_get_by_name(GST_BIN(font_pipeline), "output");
    Result font_result;
    const bool font_installed =
        font_caps && hm::archive_watermark::Install(font_caps, 0, "/not-present/hstream-archive-font.ttf");
    if (font_sink)
      g_signal_connect(font_sink, "handoff", G_CALLBACK(OnHandoff), &font_result);
    if (font_installed)
      gst_element_set_state(font_pipeline, GST_STATE_PLAYING);
    GstBus* font_bus = gst_element_get_bus(font_pipeline);
    // Dropping a buffer still returns GST_FLOW_OK upstream, so both buffers are
    // pushed and EOS follows. Drain to EOS rather than stopping at the first
    // error: the bug only showed on the second buffer, which reused the atlas
    // the failed build had left behind.
    bool saw_error = false;
    bool saw_eos = false;
    bool named_cause = false;
    bool named_remedy = false;
    GstMessage* font_message = nullptr;
    while (font_installed && !saw_eos &&
           (font_message = gst_bus_timed_pop_filtered(
                font_bus, 10 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS)))) {
      if (GST_MESSAGE_TYPE(font_message) == GST_MESSAGE_ERROR) {
        saw_error = true;
        // The point of naming the status is that an operator can act on it, so
        // check the text, not just that something failed.
        GError* reported = nullptr;
        gst_message_parse_error(font_message, &reported, nullptr);
        if (reported && reported->message) {
          const std::string text = reported->message;
          named_cause =
              text.find(
                  hm::draw_display::analytics::ToString(hm::draw_display::analytics::RenderStatus::kFontUnavailable)) !=
              std::string::npos;
          named_remedy = text.find("fonts-dejavu-core") != std::string::npos;
        }
        if (reported)
          g_error_free(reported);
      }
      saw_eos = GST_MESSAGE_TYPE(font_message) == GST_MESSAGE_EOS;
      gst_message_unref(font_message);
    }
    // Every buffer is dropped, so nothing reaches the sink and the branch
    // errors instead of publishing an unmarked archive.
    const bool font_ok = font_installed && saw_error && named_cause && named_remedy && !font_result.received;
    if (!font_ok) {
      std::cerr << "Missing archive watermark font did not stop the archive: received=" << font_result.received
                << " error=" << saw_error << " cause=" << named_cause << " remedy=" << named_remedy
                << " eos=" << saw_eos << '\n';
    }
    gst_object_unref(font_bus);
    gst_element_set_state(font_pipeline, GST_STATE_NULL);
    if (font_caps)
      gst_object_unref(font_caps);
    if (font_sink)
      gst_object_unref(font_sink);
    gst_object_unref(font_pipeline);
    if (!font_ok)
      return 1;
  }

#if defined(__aarch64__)
  // Exercise both real Jetson EGL plane layouts between nvvideoconvert and an
  // archive encoder, then inspect the resulting YUV luma on the test branch.
  for (const char* format : {"P010_10LE", "I420"}) {
    error = nullptr;
    const std::string description = std::string("videotestsrc pattern=black num-buffers=1 ! ") +
        "video/x-raw,format=I420,width=640,height=360,framerate=30/1 ! " +
        "nvvideoconvert ! video/x-raw(memory:NVMM),format=" + format +
        ",width=640,height=360 ! capsfilter name=archive_caps ! "
        "fakesink name=gpu_output signal-handoffs=true sync=false";
    pipeline = gst_parse_launch(description.c_str(), &error);
    if (!pipeline || error) {
      std::cerr << "Could not create Jetson " << format << " archive fixture\n";
      if (error)
        g_error_free(error);
      if (pipeline)
        gst_object_unref(pipeline);
      return 1;
    }
    caps_filter = gst_bin_get_by_name(GST_BIN(pipeline), "archive_caps");
    sink = gst_bin_get_by_name(GST_BIN(pipeline), "gpu_output");
    Result gpu_result;
    gpu_result.ten_bit = std::string(format) == "P010_10LE";
    const bool gpu_installed = caps_filter && hm::archive_watermark::Install(caps_filter, 0);
    if (sink)
      g_signal_connect(sink, "handoff", G_CALLBACK(OnGpuHandoff), &gpu_result);
    if (gpu_installed)
      gst_element_set_state(pipeline, GST_STATE_PLAYING);
    bus = gst_element_get_bus(pipeline);
    message = gpu_installed
        ? gst_bus_timed_pop_filtered(
              bus, 10 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))
        : nullptr;
    const bool gpu_ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS && gpu_result.received &&
        gpu_result.changed && gpu_result.chroma_changed && gpu_result.outside_unchanged;
    if (!gpu_ok) {
      if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError* bus_error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &bus_error, &debug);
        std::cerr << "Jetson " << format
                  << " archive watermark failed: " << (bus_error ? bus_error->message : "unknown") << '\n';
        if (bus_error)
          g_error_free(bus_error);
        g_free(debug);
      } else {
        std::cerr << "Jetson " << format << " archive watermark did not complete\n";
      }
    }
    if (message)
      gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (caps_filter)
      gst_object_unref(caps_filter);
    if (sink)
      gst_object_unref(sink);
    gst_object_unref(pipeline);
    if (!gpu_ok)
      return 1;
  }
  return 0;
#else
  return 0;
#endif
}
