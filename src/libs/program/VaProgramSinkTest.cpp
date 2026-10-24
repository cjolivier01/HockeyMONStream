#include "hstream/src/libs/program/VaProgramSink.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <fcntl.h>
#include <gbm.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>
#include <unistd.h>

#include <future>
#include <thread>

#include "hstream/src/libs/gpu/vaapi/VaElement.h"
#include "hstream/src/libs/gpu/vaapi/VaHipFrame.h"
#include "hstream/src/libs/media/Mp4VideoTrack.h"
#include "hstream/src/libs/media/TestMp4.h"
#include "hstream/src/libs/program/VaProgramOutput.h"

namespace {
using hm::media::test::Check;
using Buffer = std::unique_ptr<GstBuffer, decltype(&gst_buffer_unref)>;
void Status(const absl::Status& status) {
  Check(status.ok(), status.ToString());
}
void Hip(hipError_t status) {
  Check(status == hipSuccess, hipGetErrorString(status));
}
template <class T>
T Take(absl::StatusOr<T> result) {
  Status(result.status());
  return std::move(*result);
}
struct Graphics {
  int fd{-1};
  gbm_device* gbm{};
  EGLDisplay egl{EGL_NO_DISPLAY};
  GstGLDisplayEGL* display{};
  GstContext* context{};
  Graphics() {
    char pci[32]{};
    Hip(hipDeviceGetPCIBusId(pci, sizeof(pci), 0));
    const auto path = std::string("/dev/dri/by-path/pci-") + pci + "-render";
    fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    Check(fd >= 0, "Cannot open test render node");
    gbm = gbm_create_device(fd);
    Check(gbm, "Cannot create test GBM display");
    egl = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
    EGLint major{}, minor{};
    Check(eglInitialize(egl, &major, &minor), "Cannot initialize test EGL");
    display = gst_gl_display_egl_new_with_egl_display(egl);
    Check(display, "Cannot wrap test EGL display");
    context = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
    gst_context_set_gl_display(context, GST_GL_DISPLAY(display));
  }
  ~Graphics() {
    if (context)
      gst_context_unref(context);
    if (display)
      gst_object_unref(display);
    if (egl != EGL_NO_DISPLAY)
      eglTerminate(egl);
    if (gbm)
      gbm_device_destroy(gbm);
    if (fd >= 0)
      close(fd);
  }
};
struct Tile {
  hipStream_t stream{};
  void* data{};
  Tile() {
    Hip(hipStreamCreateWithFlags(&stream, hipStreamNonBlocking));
    const uint8_t rgba[] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
    Hip(hipMalloc(&data, sizeof(rgba)));
    // This bounded synthetic upload exists only in the hardware test.
    Hip(hipMemcpyAsync(data, rgba, sizeof(rgba), hipMemcpyHostToDevice, stream));
  }
  ~Tile() {
    (void)hipStreamSynchronize(stream);
    (void)hipFree(data);
    (void)hipStreamDestroy(stream);
  }
  void Render(GstBuffer* buffer, const GstVideoInfo& info) {
    auto frame = Take(hm::gpu::VaHipFrame::Import(buffer, stream, hm::gpu::VaHipFrame::Access::kWrite));
    const auto output = Take(hm::playcropper::DescribeVaProgramOutput(*frame, info));
    const hm::gpu::Image image{data, nullptr, 8, 0, 2, 2, hm::gpu::PixelFormat::kRgba8, 0, 0, 0, 0, 0, 0, 0};
    const hm::playcropper::FrameTransformGeometry transform{
        hm::BBox(0, 0, 2, 2), hm::Point{0, 0}, hm::BBox(0, 0, 1, 1), 0};
    Hip(hm::playcropper::RenderProgramYuv(image, output, transform, 0, false, 0, stream));
    Status(frame->Complete());
  }
};
} // namespace
int main(int argc, char** argv) {
  g_setenv("GST_GL_WINDOW", "surfaceless", TRUE);
  g_setenv("GST_GL_PLATFORM", "egl", TRUE);
  g_setenv("GST_GL_API", "gles2", TRUE);
  gst_init(&argc, &argv);
  try {
    Hip(hipSetDevice(0));
    Tile tile;
    GstVideoInfo info{};
    Check(gst_video_info_set_format(&info, GST_VIDEO_FORMAT_NV12, 384, 192), "Cannot create test format");
    info.fps_n = 25;
    info.fps_d = 1;
    info.chroma_site = GST_VIDEO_CHROMA_SITE_JPEG;
    Check(gst_video_colorimetry_from_string(&info.colorimetry, "bt709"), "Cannot select test color");
    hm::program::VaProgramSinkConfig config{
        hm::program::VaProgramSinkKind::kDiscard,
        0,
        info,
        2,
        10 * GST_SECOND,
        5 * GST_MSECOND,
        false,
        -1,
        1000,
        25,
        4 * 1024 * 1024,
        0,
        nullptr};
    const auto frames = [&](hm::program::VaProgramSink& sink) {
      for (unsigned i = 0; i < 5; ++i) {
        Buffer buffer(Take(sink.Acquire()), gst_buffer_unref);
        tile.Render(buffer.get(), sink.video_info());
        Status(sink.Push(buffer.release(), i * 40 * GST_MSECOND, 40 * GST_MSECOND));
      }
      Status(sink.Finish());
      Status(sink.Finish());
      Check(!sink.Acquire().ok(), "Output accepted a surface after EOS");
    };
    {
      auto pool = Take(hm::gpu::VaOutputPool::Create(0, info, 2));
      auto* selected = hm::gpu::MakeVaElement("h265enc", pool->render_node());
      Check(selected, "No HEVC encoder for the selected HIP device");
      gchar* device{};
      g_object_get(selected, "device-path", &device, nullptr);
      Check(device && std::filesystem::canonical(device) == pool->render_node(), "VA factory selected another GPU");
      g_free(device);
      gst_object_unref(selected);
      Check(!hm::gpu::MakeVaElement("h265enc", "/missing/render-node"), "Unknown render node fell back to default GPU");
    }
    auto sink = Take(hm::program::VaProgramSink::Create(config));
    frames(*sink);
    sink.reset();
    {
      auto blocked = Take(hm::program::VaProgramSink::Create(config));
      Buffer a(Take(blocked->Acquire()), gst_buffer_unref), b(Take(blocked->Acquire()), gst_buffer_unref);
      auto pending = std::async(std::launch::async, [&] { return blocked->Acquire().status(); });
      Check(pending.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout, "Pool bound was exceeded");
      blocked->Cancel();
      Check(
          pending.wait_for(std::chrono::seconds(1)) == std::future_status::ready && absl::IsCancelled(pending.get()),
          "Blocked output acquisition did not cancel promptly");
      blocked.reset();
      tile.Render(a.get(), info);
      tile.Render(b.get(), info);
    }
    {
      auto limited = config;
      limited.operation_timeout = 40 * GST_MSECOND;
      auto blocked = Take(hm::program::VaProgramSink::Create(limited));
      Buffer a(Take(blocked->Acquire()), gst_buffer_unref), b(Take(blocked->Acquire()), gst_buffer_unref);
      Check(absl::IsDeadlineExceeded(blocked->Acquire().status()), "Output pool ignored its deadline");
      a.reset();
      b.reset();
      Check(!blocked->Acquire().ok(), "Timed-out output owner revived");
    }
    {
      auto invalid = Take(hm::program::VaProgramSink::Create(config));
      Buffer buffer(Take(invalid->Acquire()), gst_buffer_unref);
      Check(!invalid->Push(buffer.release(), 0, uint64_t(INT64_MAX) + 1).ok(), "Overflowing duration accepted");
      Check(!invalid->Finish().ok(), "Invalid timestamp did not retire output");
    }
    {
      auto paced = config;
      paced.realtime = true;
      auto waiting = Take(hm::program::VaProgramSink::Create(paced));
      Buffer buffer(Take(waiting->Acquire()), gst_buffer_unref);
      tile.Render(buffer.get(), info);
      Status(waiting->Push(buffer.release(), 5 * GST_SECOND, 40 * GST_MSECOND));
      auto completion = std::async(std::launch::async, [&] { return waiting->Finish(); });
      Check(
          completion.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout,
          "Clocked output unexpectedly finished before its future frame");
      waiting->Cancel();
      Check(
          completion.wait_for(std::chrono::seconds(1)) == std::future_status::ready &&
              absl::IsCancelled(completion.get()),
          "EOS wait did not cancel promptly");
      Check(!waiting->Finish().ok(), "Cancelled EOS was reported as completion");
    }
    hm::media::test::Directory root;
    for (const auto format : {GST_VIDEO_FORMAT_NV12, GST_VIDEO_FORMAT_P010_10LE}) {
      const auto path = root.path / (format == GST_VIDEO_FORMAT_NV12 ? "eight.mp4" : "ten.mp4");
      auto encoded = config;
      encoded.kind = hm::program::VaProgramSinkKind::kHevcMp4;
      Check(gst_video_info_set_format(&encoded.video, format, 384, 192), "Invalid test format");
      encoded.video.fps_n = 25;
      encoded.video.fps_d = 1;
      encoded.video.colorimetry = info.colorimetry;
      encoded.video.chroma_site = GST_VIDEO_CHROMA_SITE_JPEG;
      encoded.output_fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
      Check(encoded.output_fd >= 0, "Cannot create test MP4");
      auto writer = hm::program::VaProgramSink::Create(encoded);
      close(encoded.output_fd);
      sink = Take(std::move(writer));
      frames(*sink);
      sink.reset();
      const auto meta = Take(hm::media::ReadMp4VideoTrack(path, 10 * GST_SECOND, {}));
      Check(
          meta.frames == 5 && meta.codec == hm::media::VideoCodec::kHevc && meta.width == 384 && meta.height == 192 &&
              meta.duration_ns == 200 * GST_MSECOND,
          "Mux EOS did not close the five-frame MP4");
      encoded.output_fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
      const auto existing = hm::program::VaProgramSink::Create(encoded);
      close(encoded.output_fd);
      Check(!existing.ok(), "Output owner accepted a nonempty file");
    }
    {
      Graphics graphics;
      auto preview = config;
      preview.kind = hm::program::VaProgramSinkKind::kPreview;
      preview.pool_frames = 1;
      preview.gl_context = graphics.context;
      Check(
          !hm::program::VaProgramSink::Create(preview).ok(),
          "Single-surface preview cannot replace its retained picture");
      preview.pool_frames = 2;
      auto preview_sink = Take(hm::program::VaProgramSink::Create(preview));
      frames(*preview_sink);
    }
    std::cout << "Program output bounds, cancellation, deadlines, HEVC mux EOS and GL preview passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
