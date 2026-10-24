#include "hstream/src/libs/program/VaProgramSink.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <fcntl.h>
#include <gbm.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>
#include <unistd.h>

#include <cmath>
#include <fstream>
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
  hm::media::test::Directory audio_environment;
  const auto alsa = audio_environment.path / "asound.conf";
  {
    std::ofstream config(alsa);
    // The unrestricted ALSA null device advertises DSD rates through UINT_MAX;
    // GStreamer 1.24's DSD probing overflows and loops forever. A PCM-only
    // conversion wrapper exercises ALSA without that unrelated driver probe.
    config << "</usr/share/alsa/alsa.conf>\npcm.hstream_null { type linear slave { pcm null format S16_LE } }\n";
    Check(bool(config), "Cannot prepare bounded ALSA test device");
  }
  g_setenv("ALSA_CONFIG_PATH", alsa.c_str(), TRUE);
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
    Status(sink->SetPaused(true));
    Check(!sink->Acquire().ok() && !sink->Finish().ok(), "Paused output accepted submission/EOS");
    Status(sink->SetPaused(false));
    {
      Buffer before_seek(Take(sink->Acquire()), gst_buffer_unref);
      tile.Render(before_seek.get(), info);
      Status(sink->Push(before_seek.release(), GST_SECOND, 40 * GST_MSECOND));
      Status(sink->ResetTimeline());
    }
    frames(*sink);
    Check(!sink->SetPaused(true).ok(), "Finished output accepted pause");
    Check(!sink->SetAudioMuted(true).ok(), "Discard output accepted preview mute");
    Check(!sink->SetPreviewVisible(true, 0).ok() && !sink->PreviewReady().ok(), "Discard output claimed a preview");
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
      auto clocked = Take(hm::program::VaProgramSink::Create(paced));
      for (unsigned i = 0; i < 2; ++i) {
        Buffer buffer(Take(clocked->Acquire()), gst_buffer_unref);
        tile.Render(buffer.get(), info);
        Status(clocked->Push(buffer.release(), i * 500 * GST_MSECOND, 40 * GST_MSECOND));
      }
      Status(clocked->SetPaused(true));
      std::this_thread::sleep_for(std::chrono::milliseconds(600));
      Status(clocked->SetPaused(false));
      const auto resumed = std::chrono::steady_clock::now();
      Status(clocked->Finish());
      Check(
          std::chrono::steady_clock::now() - resumed >= std::chrono::milliseconds(250),
          "Paused wall time consumed the video clock");
    }
    {
      auto paced = config;
      paced.realtime = true;
      auto waiting = Take(hm::program::VaProgramSink::Create(paced));
      Buffer buffer(Take(waiting->Acquire()), gst_buffer_unref);
      tile.Render(buffer.get(), info);
      Status(waiting->Push(buffer.release(), 5 * GST_SECOND, 40 * GST_MSECOND));
      Status(waiting->SetPaused(true));
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      Status(waiting->SetPaused(false));
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
    {
      auto audio_config = config;
      audio_config.realtime = true;
      audio_config.audio = hm::program::VaProgramAudioConfig{{48000, 2}, 4096, 192000, "", false};
      auto blocked = Take(hm::program::VaProgramSink::Create(audio_config));
      auto pending = std::async(std::launch::async, [&] {
        for (int64_t i = 0; i < 1000; ++i) {
          const auto status = blocked->PushAudio({i * 1024, {48000, 2}, std::vector<float>(2048)});
          if (!status.ok())
            return status;
        }
        return absl::OkStatus();
      });
      Check(
          pending.wait_for(std::chrono::milliseconds(40)) == std::future_status::timeout,
          "Unpaced audio escaped the queue bound");
      blocked->Cancel();
      Check(
          pending.wait_for(std::chrono::seconds(1)) == std::future_status::ready && absl::IsCancelled(pending.get()),
          "Blocked audio push did not cancel promptly");
    }
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
      Check(!sink->ResetTimeline().ok(), "Archive output accepted a seek");
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
    for (const auto& rates : {std::pair<int, int>{48000, 60}, {8000, 60}, {48000, 1}}) {
      const int rate = rates.first, fps = rates.second;
      const int seconds = fps == 1 ? 5 : 2;
      const auto path = root.path / ("audio-" + std::to_string(rate) + "-" + std::to_string(fps) + ".mp4");
      std::cout << "Testing continuous HEVC/AAC output at " << rate << " Hz / " << fps << " fps\n" << std::flush;
      auto encoded = config;
      encoded.kind = hm::program::VaProgramSinkKind::kHevcMp4;
      encoded.video.fps_n = fps;
      // Use intra-only low-rate input: this Mesa driver emits a filler-only
      // P access unit at 1 fps independently of the audio path.
      encoded.bitrate_kbps = fps == 1 ? 100 : 1000;
      encoded.keyframe_interval = fps == 1 ? 1 : 25;
      encoded.pool_frames = 8;
      encoded.audio = hm::program::VaProgramAudioConfig{{rate, 2}, 8192, 192000, "", false};
      // Smaller than AAC preroll at either rate: encoded buffering must free
      // surfaces while the sequential producer submits the required PCM.
      encoded.pool_frames = 2;
      encoded.output_fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
      Check(encoded.output_fd >= 0, "Cannot create audio test MP4");
      Check(!hm::program::VaProgramSink::Create(encoded).ok(), "AAC finalizer accepted a write-only descriptor");
      close(encoded.output_fd);
      encoded.output_fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
      Check(encoded.output_fd >= 0, "Cannot reopen audio test MP4");
      sink = Take(hm::program::VaProgramSink::Create(encoded));
      close(encoded.output_fd);
      for (int64_t i = 0; i < seconds * fps; ++i) {
        auto acquired = sink->Acquire();
        Check(
            acquired.ok(),
            acquired.status().ToString() + " at frame " + std::to_string(i) + " rate " + std::to_string(rate));
        Buffer buffer(*acquired, gst_buffer_unref);
        tile.Render(buffer.get(), encoded.video);
        const auto start = gst_util_uint64_scale(i, GST_SECOND, fps);
        const auto end = gst_util_uint64_scale(i + 1, GST_SECOND, fps);
        Status(sink->Push(buffer.release(), start, end - start));
        const auto first_sample = i * rate / fps;
        const auto last_sample = (i + 1) * rate / fps;
        for (auto sample = first_sample; sample < last_sample;) {
          const auto sample_count = std::min<int64_t>(4096, last_sample - sample);
          hm::media::AudioFrame audio{sample, {rate, 2}, std::vector<float>(sample_count * 2)};
          for (int j = 0; j < sample_count; ++j)
            audio.pcm[j * 2] = audio.pcm[j * 2 + 1] = .1f * std::sin((audio.sample + j) * 2 * M_PI * 997 / rate);
          Status(sink->PushAudio(audio));
          sample += sample_count;
        }
      }
      Status(sink->Finish());
      Status(sink->Finish());
      sink.reset();
      const auto video = Take(hm::media::ReadMp4VideoTrack(path, 10 * GST_SECOND, {}));
      Check(
          video.frames == uint64_t(seconds * fps) && video.duration_ns == seconds * GST_SECOND,
          "Audio changed encoded video duration");
      auto reader = Take(hm::media::Mp4AudioReader::Create({10 * GST_SECOND, 1024 * 1024, 4096}));
      Status(reader->Open(path));
      Status(reader->Seek(0));
      auto audio = Take(reader->Read());
      Check(audio && audio->sample == 0, "Encoded AAC retained audible priming");
      double error = 0;
      size_t count = 0;
      while (audio) {
        for (size_t i = 0; i < audio->samples(); ++i) {
          const auto sample = audio->sample + i;
          if (sample >= size_t(seconds * rate))
            break;
          error += std::abs(audio->pcm[i * 2] - .1 * std::sin(sample * 2 * M_PI * 997 / rate));
          ++count;
        }
        audio = Take(reader->Read());
      }
      Check(count == size_t(seconds * rate) && error / count < .005, "AAC edit shifted or lost audible samples");
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
      Check(!Take(preview_sink->PreviewReady()), "Preview ready before its first frame");
      Check(!preview_sink->SetPreviewVisible(true, GST_CLOCK_TIME_NONE).ok(), "Invalid preview time accepted");
      const auto submit = [&](int frame) {
        Buffer buffer(Take(preview_sink->Acquire()), gst_buffer_unref);
        tile.Render(buffer.get(), preview.video);
        Status(preview_sink->Push(buffer.release(), frame * 40 * GST_MSECOND, 40 * GST_MSECOND));
      };
      const auto wait_ready = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!Take(preview_sink->PreviewReady()) && std::chrono::steady_clock::now() < deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Check(Take(preview_sink->PreviewReady()), "No completed GL draw for the selected preview epoch");
      };
      Status(preview_sink->SetPreviewVisible(false, 0));
      for (int i = 0; i < 5; ++i)
        submit(i);
      Check(!Take(preview_sink->PreviewReady()), "Hidden preview became ready");
      Status(preview_sink->SetPreviewVisible(true, 5 * 40 * GST_MSECOND));
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      Check(!Take(preview_sink->PreviewReady()), "Queued old frames acknowledged a new preview epoch");
      for (int i = 5; i < 10; ++i)
        submit(i);
      wait_ready();
      Status(preview_sink->SetPreviewVisible(false, 10 * 40 * GST_MSECOND));
      submit(10);
      Check(!Take(preview_sink->PreviewReady()), "Disabled preview retained its old readiness");
      Status(preview_sink->ResetTimeline());
      Check(!Take(preview_sink->PreviewReady()), "Timeline reset lost hidden preview state");
      Status(preview_sink->SetPreviewVisible(true, 0));
      frames(*preview_sink);
      wait_ready();
      Check(!preview_sink->SetPreviewVisible(false, 0).ok(), "Completed preview accepted visibility changes");
      preview_sink.reset();
      preview.audio = hm::program::VaProgramAudioConfig{{48000, 2}, 8192, 192000, "hstream_null", false};
      std::cout << "Testing clocked GL/ALSA preview\n" << std::flush;
      preview.realtime = true;
      preview_sink = Take(hm::program::VaProgramSink::Create(preview));
      for (int64_t i = 0; i < 5; ++i) {
        if (i == 2) {
          Status(preview_sink->SetPaused(true));
          Status(preview_sink->SetAudioMuted(true));
          std::this_thread::sleep_for(std::chrono::milliseconds(80));
          Status(preview_sink->SetAudioMuted(false));
          Status(preview_sink->SetPaused(false));
        }
        Buffer buffer(Take(preview_sink->Acquire()), gst_buffer_unref);
        tile.Render(buffer.get(), preview.video);
        Status(preview_sink->Push(buffer.release(), i * 40 * GST_MSECOND, 40 * GST_MSECOND));
        Status(preview_sink->PushAudio({i * 1920, {48000, 2}, std::vector<float>(3840)}));
      }
      // Both PCM sample zero and video timestamp zero must be accepted after
      // flushing an active GL/ALSA timeline, without recreating the owner.
      Status(preview_sink->ResetTimeline());
      Check(!Take(preview_sink->PreviewReady()), "Seek retained preview readiness from the old timeline");
      for (int64_t i = 0; i < 5; ++i) {
        Buffer buffer(Take(preview_sink->Acquire()), gst_buffer_unref);
        tile.Render(buffer.get(), preview.video);
        Status(preview_sink->Push(buffer.release(), i * 40 * GST_MSECOND, 40 * GST_MSECOND));
        Status(preview_sink->PushAudio({i * 1920, {48000, 2}, std::vector<float>(3840)}));
      }
      Status(preview_sink->Finish());
    }
    std::cout << "Program output bounds, cancellation, deadlines, HEVC mux EOS and GL preview passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
