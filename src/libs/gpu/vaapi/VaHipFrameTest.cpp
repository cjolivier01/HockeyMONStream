#include "hstream/src/libs/gpu/vaapi/VaHipFrame.h"

#include "hstream/src/libs/draw_display/AnalyticsOverlay.h"

#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
void Check(bool value, const std::string& message) {
  if (!value) {
    std::cerr << message << std::endl;
    std::abort();
  }
}
void Hip(hipError_t value) {
  Check(value == hipSuccess, hipGetErrorString(value));
}
void Ok(const absl::Status& status) {
  Check(status.ok(), status.ToString());
}

struct Run {
  hipStream_t stream{};
  int device{};
  bool ten_bit{};
  hm::draw_display::analytics::Compositor compositor;
  hm::draw_display::analytics::CommandList commands;
  std::vector<GstClockTime> input_times, output_times;
};

size_t DescriptorCount();

void Process(GstElement*, GstBuffer* buffer, gpointer data) {
  auto& run = *static_cast<Run*>(data);
  Hip(hipSetDevice(run.device));
  // A tee/shared buffer must never be changed in place.
  GstBuffer* shared = gst_buffer_ref(buffer);
  Check(
      !hm::gpu::VaHipFrame::Import(buffer, run.stream, hm::gpu::VaHipFrame::Access::kWrite).ok(),
      "A shared VA buffer was accepted for writing");
  gst_buffer_unref(shared);
  auto imported = hm::gpu::VaHipFrame::Import(buffer, run.stream, hm::gpu::VaHipFrame::Access::kWrite);
  Check(imported.ok(), imported.status().ToString());
  auto frame = std::move(imported).value();
  Check(frame->width() == 384 && frame->height() == 192 && frame->plane_count() == 2, "Wrong imported layout");
  const auto& y = frame->plane(0);
  const auto& uv = frame->plane(1);
  const unsigned patch_x = (run.input_times.size() % 4) * 64;
  if (run.ten_bit) {
    Check(frame->format() == hm::gpu::VaHipFrame::Format::kP010, "Expected P010");
    using namespace hm::draw_display::analytics;
    run.commands.Clear();
    Check(run.commands.AddFill(patch_x, 0, patch_x + 64, 64, {1, 1, 1, 1}), "Cannot create test overlay");
    ImageView view{y.data, y.pitch, frame->width(), frame->height(), PixelFormat::kP010, uv.data, uv.pitch};
    const auto result = run.compositor.Render(view, run.commands, run.stream);
    Check(result.status == RenderStatus::kOk && result.launched, "HIP compositor did not draw on the VA surface");
  } else {
    Check(frame->format() == hm::gpu::VaHipFrame::Format::kNv12, "Expected NV12");
    Hip(hipMemset2DAsync(static_cast<uint8_t*>(y.data) + patch_x, y.pitch, 235, 64, 64, run.stream));
    Hip(hipMemset2DAsync(static_cast<uint8_t*>(uv.data) + patch_x, uv.pitch, 128, 64, 32, run.stream));
  }
  // Alternate explicit completion and the destructor's early-return drain.
  if (run.input_times.size() % 2) {
    Ok(frame->Complete());
    Ok(frame->Complete());
  }
  frame.reset();
  if (run.input_times.size() == 31) {
    // Isolate adapter ownership from the media driver's pipeline caches. The
    // same retained surface must tolerate repeated import/drain/release.
    const auto before = DescriptorCount();
    for (int i = 0; i < 32; ++i) {
      auto again = hm::gpu::VaHipFrame::Import(buffer, run.stream, hm::gpu::VaHipFrame::Access::kRead);
      Check(again.ok(), again.status().ToString());
      Ok((*again)->Complete());
    }
    Check(DescriptorCount() <= before, "VA/HIP imports leaked file descriptors");
  }
  run.input_times.push_back(GST_BUFFER_PTS(buffer));
}

GstFlowReturn ReadResult(GstAppSink* sink, gpointer data) {
  auto& run = *static_cast<Run*>(data);
  GstSample* sample = gst_app_sink_pull_sample(sink);
  Check(sample != nullptr, "Missing decoded output sample");
  auto* buffer = gst_sample_get_buffer(sample);
  GstVideoInfo info;
  Check(gst_video_info_from_caps(&info, gst_sample_get_caps(sample)), "Invalid decoded output caps");
  Check(
      GST_VIDEO_INFO_FORMAT(&info) == (run.ten_bit ? GST_VIDEO_FORMAT_P010_10LE : GST_VIDEO_FORMAT_NV12),
      "Wrong test readback format");
  // Test-only, bounded 384x192 readback after hardware re-encode/decode. The
  // production interop adapter never maps pixels or requests system-memory caps.
  GstVideoFrame frame;
  Check(gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ), "Cannot read test output");
  const auto* y = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
  const auto* uv = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
  const auto y_pitch = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
  const auto uv_pitch = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
  const auto value = [&](const uint8_t* plane, int pitch, int x, int row) {
    const auto* p = plane + row * pitch;
    return run.ten_bit ? static_cast<int>(reinterpret_cast<const uint16_t*>(p)[x] >> 8) : static_cast<int>(p[x]);
  };
  const unsigned patch_x = (run.output_times.size() % 4) * 64 + 32;
  const unsigned untouched_x = ((run.output_times.size() + 1) % 4) * 64 + 32;
  const int white = value(y, y_pitch, patch_x, 32);
  const int white_u = value(uv, uv_pitch, patch_x, 16);
  const int white_v = value(uv, uv_pitch, patch_x + 1, 16);
  const int blue = value(y, y_pitch, 200, 100);
  if (white < 220 || white_u < 118 || white_u > 138 || white_v < 118 || white_v > 138) {
    std::cerr << (run.ten_bit ? "P010" : "NV12") << " output YUV = " << white << "," << white_u << "," << white_v
              << std::endl;
  }
  Check(
      white > 220 && white_u > 118 && white_u < 138 && white_v > 118 && white_v < 138,
      "HIP modification missing from encoded output");
  Check(blue > 15 && blue < 65 && value(uv, uv_pitch, 200, 50) > 200, "Unmodified video region was corrupted");
  Check(value(y, y_pitch, untouched_x, 32) < 65, "Stale HIP patch survived surface reuse");
  gst_video_frame_unmap(&frame);
  run.output_times.push_back(GST_BUFFER_PTS(buffer));
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

size_t DescriptorCount() {
  size_t count = 0;
  for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator("/proc/self/fd"))
    ++count;
  return count;
}

void VideoRoundTrip(bool ten_bit) {
  Run run;
  run.ten_bit = ten_bit;
  Hip(hipGetDevice(&run.device));
  Hip(hipStreamCreateWithFlags(&run.stream, hipStreamNonBlocking));
  const std::string format = ten_bit ? "P010_10LE" : "NV12";
  const std::string drm = ten_bit ? "P010" : "NV12";
  const std::string codec = ten_bit ? "h265" : "h264";
  const std::string pipeline_text = "videotestsrc num-buffers=32 pattern=blue ! video/x-raw,format=" + format +
      ",width=384,height=192,framerate=30/1 ! vapostproc ! video/x-raw(memory:VAMemory),format=" + format + " ! va" +
      codec + "enc ! " + codec + "parse ! va" + codec +
      "dec ! vapostproc disable-passthrough=true ! video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=" + drm +
      " ! identity name=compute signal-handoffs=true ! vapostproc ! video/x-raw(memory:VAMemory),format=" + format +
      " ! va" + codec + "enc ! " + codec + "parse ! va" + codec + "dec ! video/x-raw,format=" + format +
      " ! appsink name=result sync=false emit-signals=true";
  GError* error = nullptr;
  auto* pipeline = gst_parse_launch(pipeline_text.c_str(), &error);
  Check(error == nullptr && pipeline, error ? error->message : "Cannot create test pipeline");
  auto* compute = gst_bin_get_by_name(GST_BIN(pipeline), "compute");
  auto* result = gst_bin_get_by_name(GST_BIN(pipeline), "result");
  g_signal_connect(compute, "handoff", G_CALLBACK(Process), &run);
  g_signal_connect(result, "new-sample", G_CALLBACK(ReadResult), &run);
  gst_object_unref(compute);
  gst_object_unref(result);
  Check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "Pipeline did not start");
  auto* bus = gst_element_get_bus(pipeline);
  auto* message = gst_bus_timed_pop_filtered(
      bus, 30 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
  if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    gchar* debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    std::cerr << error->message << ": " << (debug ? debug : "") << std::endl;
    g_error_free(error);
    g_free(debug);
  }
  const bool eos = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
  gst_element_set_state(pipeline, GST_STATE_NULL);
  if (message)
    gst_message_unref(message);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  Check(eos, "VA/HIP hardware round trip did not reach EOS");
  Check(run.input_times.size() == 32 && run.output_times.size() == 32, "Video frames were lost");
  Check(run.input_times == run.output_times, "Video timestamps changed");
  for (size_t i = 0; i < run.input_times.size(); ++i) {
    Check(GST_CLOCK_TIME_IS_VALID(run.input_times[i]), "Missing video timestamp");
    if (i)
      Check(run.input_times[i] > run.input_times[i - 1], "Non-increasing video timestamps");
  }
  Hip(hipStreamSynchronize(run.stream));
  Hip(hipStreamDestroy(run.stream));
  std::cout << (ten_bit ? "P010/HEVC + HStream compositor" : "NV12/H.264")
            << ": 32 frames, modified pixels, background, timestamps and EOS passed\n";
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  Hip(hipSetDevice(0));
  Check(
      !hm::gpu::VaHipFrame::Import(nullptr, nullptr, hm::gpu::VaHipFrame::Access::kRead).ok(),
      "Invalid import accepted");
  VideoRoundTrip(false);
  VideoRoundTrip(true);
}
