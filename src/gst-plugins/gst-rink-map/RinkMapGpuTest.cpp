#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include "hstream/src/gst-plugins/gst-rink-map/RinkMapOverlay.h"
#include "hstream/src/libs/stitching/ConfigureStitchingCore.h"
#if defined(HSTREAM_USE_HIP)
#include "hstream/src/libs/gpu/vaapi/VaHipFrame.h"
#include "hstream/src/libs/gpu/vaapi/VaOutputPool.h"
#else
#include <nvbufsurface.h>
#endif

namespace {
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void Gpu(cudaError_t result) {
  Check(result == cudaSuccess, cudaGetErrorString(result));
}
struct Fixture {
  GstVideoInfo info{};
  cudaStream_t stream{};
#if defined(HSTREAM_USE_HIP)
  std::unique_ptr<hm::gpu::VaOutputPool> pool;
#endif
  Fixture(unsigned width, unsigned height) {
    gst_video_info_set_format(&info, GST_VIDEO_FORMAT_P010_10LE, width, height);
    gst_video_colorimetry_from_string(&info.colorimetry, "bt709");
    info.fps_n = 30;
    info.fps_d = 1;
    info.chroma_site = GST_VIDEO_CHROMA_SITE_JPEG;
    Gpu(cudaStreamCreate(&stream));
#if defined(HSTREAM_USE_HIP)
    auto result = hm::gpu::VaOutputPool::Create(0, info, 8);
    Check(result.ok(), result.status().ToString().c_str());
    pool = std::move(*result);
#endif
  }
  ~Fixture() {
    cudaStreamDestroy(stream);
  }
  GstCaps* Caps() {
#if defined(HSTREAM_USE_HIP)
    return gst_caps_ref(pool->caps());
#else
    auto* caps = gst_video_info_to_caps(&info);
    gst_caps_set_features(caps, 0, gst_caps_features_new("memory:NVMM", nullptr));
    return caps;
#endif
  }
  GstBuffer* Buffer() {
#if defined(HSTREAM_USE_HIP)
    auto result = pool->Acquire();
    Check(result.ok(), result.status().ToString().c_str());
    auto* buffer = *result;
    auto imported = hm::gpu::VaHipFrame::Import(buffer, stream, hm::gpu::VaHipFrame::Access::kWrite);
    Check(imported.ok(), imported.status().ToString().c_str());
    for (unsigned i = 0; i < (*imported)->plane_count(); ++i) {
      const auto& plane = (*imported)->plane(i);
      Gpu(hipMemset2DAsync(plane.data, plane.pitch, 0, plane.row_bytes, plane.height, stream));
    }
    Check((*imported)->Complete().ok(), "VA fixture fill failed");
    return buffer;
#else
    NvBufSurfaceCreateParams params{};
    params.width = info.width;
    params.height = info.height;
    params.colorFormat = NVBUF_COLOR_FORMAT_NV12_10LE_709;
    params.layout = NVBUF_LAYOUT_PITCH;
    params.memType = NVBUF_MEM_CUDA_DEVICE;
    NvBufSurface* surface = nullptr;
    Check(NvBufSurfaceCreate(&surface, 1, &params) == 0, "NV fixture allocation failed");
    surface->numFilled = 1;
    Gpu(cudaMemset(surface->surfaceList[0].dataPtr, 0, surface->surfaceList[0].dataSize));
    return gst_buffer_new_wrapped_full(
        GstMemoryFlags(0), surface, sizeof(*surface), 0, sizeof(*surface), surface, [](gpointer p) {
          NvBufSurfaceDestroy(static_cast<NvBufSurface*>(p));
        });
#endif
  }
  std::vector<uint16_t> Read(GstBuffer* buffer) {
    std::vector<uint16_t> pixels(info.width * info.height);
#if defined(HSTREAM_USE_HIP)
    auto imported = hm::gpu::VaHipFrame::Import(buffer, stream, hm::gpu::VaHipFrame::Access::kRead);
    Check(imported.ok(), imported.status().ToString().c_str());
    const auto& plane = (*imported)->plane(0);
    Gpu(cudaMemcpy2DAsync(
        pixels.data(),
        info.width * 2,
        plane.data,
        plane.pitch,
        info.width * 2,
        info.height,
        cudaMemcpyDeviceToHost,
        stream));
    Check((*imported)->Complete().ok(), "VA fixture read failed");
#else
    GstMapInfo map{};
    Check(gst_buffer_map(buffer, &map, GST_MAP_READ), "NV descriptor map failed");
    const auto& p = reinterpret_cast<NvBufSurface*>(map.data)->surfaceList[0];
    Gpu(cudaMemcpy2D(
        pixels.data(),
        info.width * 2,
        static_cast<uint8_t*>(p.dataPtr) + p.planeParams.offset[0],
        p.planeParams.pitch[0],
        info.width * 2,
        info.height,
        cudaMemcpyDeviceToHost));
    gst_buffer_unmap(buffer, &map);
#endif
    return pixels;
  }
};
void Run(unsigned width, unsigned height) {
  Fixture fixture(width, height);
  auto map = std::make_shared<hm::stitching::PlaybackRinkMap>();
  map->hugin_generation = "fixture\n";
  map->fit.state = hm::stitching::RinkMapState::kReady;
  map->fit.plane_to_ray = cv::Matx33d(0, -1, 19, -1, 0, -3, 0, 0, -7);
  map->columns = map->rows = 2;
  map->rays.assign(4, *hm::stitching::RinkPointToRay(map->fit, {0, 0}));
  hm::draw_display::RinkMapFrame frame;
  frame.generation = *hm::stitching::stitched_output_generation_id(map->hugin_generation, 0, 1600, 1200);
  frame.stitched_width = 1600;
  frame.stitched_height = 1200;
  frame.coordinate_width = 800;
  frame.coordinate_height = 600;
  frame.have_tracks = true;
  frame.players.push_back({350, 100, 100, 200, {.2F, .4F, .7F, 1}, 22, false});
  auto filter =
      hm::rink_map::Create({{true, hm::draw_display::RinkMiniMapPreferences("compact", "upper-right"), 8}, map, 0});
  Check(filter.ok(), filter.status().ToString().c_str());
  auto* pipeline = gst_pipeline_new(nullptr);
  auto* source = gst_element_factory_make("appsrc", nullptr);
  auto* sink = gst_element_factory_make("appsink", nullptr);
  auto* tee = gst_element_factory_make("tee", nullptr);
  auto* sample_queue = gst_element_factory_make("queue", nullptr);
  auto* encode_queue = gst_element_factory_make("queue", nullptr);
#if defined(HSTREAM_USE_HIP)
  auto* encoder = gst_element_factory_make("vah265enc", nullptr);
#else
  auto* encoder = gst_element_factory_make("nvv4l2h265enc", nullptr);
#endif
  auto* parser = gst_element_factory_make("h265parse", nullptr);
  auto* encoded = gst_element_factory_make("fakesink", nullptr);
  Check(encoder && parser && encoded, "Hardware HEVC encoder required for output regression");
  std::atomic<unsigned> packets{0};
  g_object_set(encoded, "sync", FALSE, "async", FALSE, "signal-handoffs", TRUE, nullptr);
  g_signal_connect(
      encoded,
      "handoff",
      G_CALLBACK(
          +[](GstElement*, GstBuffer*, GstPad*, gpointer data) { ++*static_cast<std::atomic<unsigned>*>(data); }),
      &packets);
  gst_bin_add_many(
      GST_BIN(pipeline), source, *filter, tee, sample_queue, sink, encode_queue, encoder, parser, encoded, nullptr);
  Check(
      gst_element_link_many(source, *filter, tee, nullptr) && gst_element_link_many(tee, sample_queue, sink, nullptr) &&
          gst_element_link_many(tee, encode_queue, encoder, parser, encoded, nullptr),
      "Cannot link rink-map encoder pipeline");
  auto* caps = fixture.Caps();
  gst_app_src_set_caps(GST_APP_SRC(source), caps);
  gst_caps_unref(caps);
  g_object_set(source, "format", GST_FORMAT_TIME, nullptr);
  g_object_set(sink, "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE, nullptr);
#if defined(HSTREAM_USE_HIP)
  fixture.pool->SetDisplayContext(pipeline);
#endif
  Check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "Cannot play rink map");
  const auto begin = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < 32; ++i) {
    auto* input = fixture.Buffer();
    frame.pts = i * GST_SECOND / 30;
    frame.sequence = i;
    if (i == 31)
      frame.generation = "stale";
    Check(hm::rink_map::AttachFrame(input, frame, fixture.info), "Cannot attach fixture metadata");
    GST_BUFFER_PTS(input) = frame.pts;
    GST_BUFFER_DURATION(input) = GST_SECOND / 30;
#if !defined(HSTREAM_USE_HIP)
    // Simulate a tee retaining the clean input. NVIDIA must copy pixels.
    auto* retained = gst_buffer_ref(input);
#endif
    Check(gst_app_src_push_buffer(GST_APP_SRC(source), input) == GST_FLOW_OK, "Cannot submit rink frame");
    auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND);
    if (!sample) {
      auto* bus = gst_element_get_bus(pipeline);
      auto* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
      if (message) {
        GError* e = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &e, &debug);
        std::cerr << (e ? e->message : "error") << ' ' << (debug ? debug : "") << '\n';
        g_clear_error(&e);
        g_free(debug);
        gst_message_unref(message);
      }
      gst_object_unref(bus);
    }
    Check(sample, "No rink-map output");
    auto* output = gst_sample_get_buffer(sample);
    Check(
        GST_BUFFER_PTS(output) == frame.pts && GST_BUFFER_DURATION(output) == GST_SECOND / 30,
        "Overlay changed timestamps");
    if (i == 0 || i == 31) {
      const auto pixels = fixture.Read(output);
      size_t changed = 0;
      for (auto p : pixels)
        changed += p != 0;
      Check(i == 0 ? changed > 100 : changed == 0, "Map pixels missing or stale calibration painted video");
      Check(pixels[0] == 0 && pixels.back() == 0, "Overlay modified unrelated video pixels");
#if !defined(HSTREAM_USE_HIP)
      const auto clean = fixture.Read(retained);
      for (auto p : clean)
        Check(p == 0, "Overlay corrupted shared tee input");
#endif
    }
#if !defined(HSTREAM_USE_HIP)
    gst_buffer_unref(retained);
#endif
    gst_sample_unref(sample);
  }
  gst_app_src_end_of_stream(GST_APP_SRC(source));
  auto* bus = gst_element_get_bus(pipeline);
  auto* eos = gst_bus_timed_pop_filtered(bus, 10 * GST_SECOND, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
  Check(eos && GST_MESSAGE_TYPE(eos) == GST_MESSAGE_EOS, "Rink map failed to drain EOS");
  gst_message_unref(eos);
  gst_object_unref(bus);
  Check(packets >= 32, "Overlay output did not reach hardware encoding");
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
  std::cout << width << 'x' << height
            << ": 32 P010 frames, pixels/ownership/timestamps/stale calibration/HEVC/EOS passed; " << elapsed
            << " seconds (includes allocations and test readback)\n";
}
} // namespace
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  try {
    Run(1280, 720);
    Run(3840, 2160);
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
