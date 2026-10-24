// Test-only bounded upload/readback around the production GPU crop and VA pool.
#include "hstream/src/libs/program/VaProgramOutput.h"
#include "hstream/src/libs/gpu/vaapi/VaHipFrame.h"
#include "hstream/src/libs/gpu/vaapi/VaOutputPool.h"
#include "hstream/src/libs/program/ProgramGpu.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {
void Check(bool ok, const std::string& message) {
  if (!ok) {
    std::cerr << message << std::endl;
    std::abort();
  }
}
void Hip(hipError_t status) {
  Check(status == hipSuccess, hipGetErrorString(status));
}
struct Results {
  std::atomic<unsigned> frames{0};
};
GstFlowReturn Read(GstAppSink* sink, gpointer data) {
  auto& results = *static_cast<Results*>(data);
  auto* sample = gst_app_sink_pull_sample(sink);
  Check(sample, "missing encoded sample");
  auto* buffer = gst_sample_get_buffer(sample);
  GstVideoInfo info{};
  Check(gst_video_info_from_caps(&info, gst_sample_get_caps(sample)), "missing decoded caps");
  // Test-only 384x192 decoded result. Neither the pool nor cropper maps video.
  GstVideoFrame frame{};
  Check(gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ), "cannot inspect encoded test frame");
  const bool ten_bit = GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_P010_10LE;
  const auto sample_code = [&](int plane, int x, int y) {
    const auto* p = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, plane)) +
        y * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, plane);
    return ten_bit ? double(reinterpret_cast<const uint16_t*>(p)[x] >> 6) / 4 : double(p[x]);
  };
  const auto sequence = results.frames.load();
  const bool red = sequence % 2 == 0;
  const double luma = red ? .2126 : .0722;
  const double expected_y = 16 + 219 * luma;
  const double expected_u = 128 + 224 * ((red ? 0.0 : 1.0) - luma) / (2 * (1 - .0722));
  const double expected_v = 128 + 224 * ((red ? 1.0 : 0.0) - luma) / (2 * (1 - .2126));
  Check(
      std::abs(sample_code(0, 192, 96) - expected_y) < 8 && std::abs(sample_code(1, 192, 48) - expected_u) < 8 &&
          std::abs(sample_code(1, 193, 48) - expected_v) < 8,
      "crop selection or encoded YUV pixels incorrect");
  // Use 25 fps (an integral nanosecond period) to isolate timestamp ownership
  // from parser fractional-frame rounding. VA encoders offset their segment to leave room for negative DTS. Compare
  // content time through the actual output segment, not that private PTS origin.
  const auto content_time =
      gst_segment_to_stream_time(gst_sample_get_segment(sample), GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
  Check(content_time == gst_util_uint64_scale(sequence, GST_SECOND, 25), "output content timestamp changed");
  gst_video_frame_unmap(&frame);
  gst_sample_unref(sample);
  ++results.frames;
  return GST_FLOW_OK;
}
} // namespace
int main(int argc, char** argv) {
  const bool ten_bit = argc == 2 && std::string(argv[1]) == "--p010";
  gst_init(&argc, &argv);
  Hip(hipSetDevice(0));
  GstVideoInfo info{};
  Check(
      gst_video_info_set_format(&info, ten_bit ? GST_VIDEO_FORMAT_P010_10LE : GST_VIDEO_FORMAT_NV12, 384, 192),
      "output format");
  info.fps_n = 25;
  info.fps_d = 1;
  info.chroma_site = GST_VIDEO_CHROMA_SITE_JPEG;
  Check(gst_video_colorimetry_from_string(&info.colorimetry, "bt709"), "output colorimetry");
  auto created = hm::gpu::VaOutputPool::Create(0, info, 4);
  Check(created.ok(), created.status().ToString());
  auto pool = std::move(*created);
  std::vector<GstBuffer*> held;
  for (int i = 0; i < 4; ++i) {
    auto frame = pool->Acquire();
    Check(frame.ok(), frame.status().ToString());
    held.push_back(*frame);
  }
  Check(absl::IsResourceExhausted(pool->Acquire().status()), "pool must be bounded and nonblocking");
  for (auto* frame : held)
    gst_buffer_unref(frame);
  info.colorimetry.transfer = GST_VIDEO_TRANSFER_SMPTE2084;
  Check(!hm::gpu::VaOutputPool::Create(0, info, 4).ok(), "HDR output requires tone mapping");

  info.colorimetry.transfer = GST_VIDEO_TRANSFER_BT709;
  GError* error{};
  const std::string codec = ten_bit ? "h265" : "h264";
  const std::string graph = "appsrc name=input format=time ! va" + codec + "enc b-frames=0 key-int-max=1 ! " + codec +
      "parse ! va" + codec + "dec ! video/x-raw,format=" + (ten_bit ? "P010_10LE" : "NV12") +
      " ! appsink name=result sync=false emit-signals=true";
  auto* pipeline = gst_parse_launch(graph.c_str(), &error);
  Check(pipeline && !error, error ? error->message : "cannot create encode pipeline");
  pool->SetDisplayContext(pipeline);
  auto* input = gst_bin_get_by_name(GST_BIN(pipeline), "input");
  auto* result = gst_bin_get_by_name(GST_BIN(pipeline), "result");
  gst_app_src_set_caps(GST_APP_SRC(input), pool->caps());
  Results results;
  g_signal_connect(result, "new-sample", G_CALLBACK(Read), &results);
  Check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "encode startup failed");
  hipStream_t stream{};
  Hip(hipStreamCreateWithFlags(&stream, hipStreamNonBlocking));
  std::vector<uint8_t> pixels(768 * 192 * 4, 0);
  for (int y = 0; y < 192; ++y)
    for (int x = 0; x < 768; ++x) {
      pixels[(y * 768 + x) * 4 + (x < 384 ? 0 : 2)] = 255;
      pixels[(y * 768 + x) * 4 + 3] = 255;
    }
  void* source{};
  Hip(hipMalloc(&source, pixels.size()));
  Hip(hipMemcpyAsync(source, pixels.data(), pixels.size(), hipMemcpyHostToDevice, stream));
  const hm::gpu::Image image{source, nullptr, 768 * 4, 0, 768, 192, hm::gpu::PixelFormat::kRgba8, 0, 0, 0, 0, 0, 0, 0};
  for (unsigned sequence = 0; sequence < 32; ++sequence) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto acquired = pool->Acquire();
    while (absl::IsResourceExhausted(acquired.status()) && std::chrono::steady_clock::now() < deadline) {
      auto* bus = gst_element_get_bus(pipeline);
      auto* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
      gst_object_unref(bus);
      if (message) {
        gst_message_parse_error(message, &error, nullptr);
        Check(false, error->message);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      acquired = pool->Acquire();
    }
    Check(acquired.ok(), acquired.status().ToString());
    auto* buffer = *acquired;
    auto imported = hm::gpu::VaHipFrame::Import(buffer, stream, hm::gpu::VaHipFrame::Access::kWrite);
    Check(imported.ok(), imported.status().ToString());
    auto frame = std::move(*imported);
    const auto output = hm::playcropper::DescribeVaProgramOutput(*frame, info);
    Check(output.ok(), output.status().ToString());
    const float left = sequence % 2 == 0 ? 0 : 384;
    const hm::playcropper::FrameTransformGeometry transform{
        hm::BBox(0, 0, 768, 192), hm::Point{0, 0}, hm::BBox(left, 0, left + 384, 192), 0};
    Hip(hm::playcropper::RenderProgramYuv(image, *output, transform, 0, false, 0, stream));
    // Exercise both explicit fence and early-return destructor fence.
    if (sequence % 2)
      Check(frame->Complete().ok(), "output stream failed");
    frame.reset();
    GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(sequence, GST_SECOND, 25);
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(sequence + 1, GST_SECOND, 25) - GST_BUFFER_PTS(buffer);
    Check(gst_app_src_push_buffer(GST_APP_SRC(input), buffer) == GST_FLOW_OK, "encode input refused frame");
  }
  Check(gst_app_src_end_of_stream(GST_APP_SRC(input)) == GST_FLOW_OK, "encode EOS refused");
  auto* bus = gst_element_get_bus(pipeline);
  auto* message = gst_bus_timed_pop_filtered(
      bus, 30 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
  if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    gchar* debug{};
    gst_message_parse_error(message, &error, &debug);
    std::cerr << error->message << ": " << (debug ? debug : "") << '\n';
    g_error_free(error);
    g_free(debug);
  }
  Check(message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS, "encode output did not reach EOS");
  Check(results.frames == 32, "encode dropped frames");
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_message_unref(message);
  gst_object_unref(bus);
  gst_object_unref(input);
  gst_object_unref(result);
  gst_object_unref(pipeline);
  Hip(hipStreamSynchronize(stream));
  Hip(hipFree(source));
  Hip(hipStreamDestroy(stream));
  // Outstanding surfaces retain their VA allocation after their pool owner dies.
  auto last = pool->Acquire();
  Check(last.ok(), last.status().ToString());
  pool.reset();
  Hip(hipStreamCreateWithFlags(&stream, hipStreamNonBlocking));
  auto retained = hm::gpu::VaHipFrame::Import(*last, stream, hm::gpu::VaHipFrame::Access::kRead);
  Check(retained.ok(), retained.status().ToString());
  Check((*retained)->Complete().ok(), "retained output completion");
  retained->reset();
  gst_buffer_unref(*last);
  Hip(hipStreamDestroy(stream));
  std::cout << codec << ": 32 Program crops, bounded pool, encoded YUV pixels, timestamps and EOS passed\n";
}
