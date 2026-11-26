#include "hstream/src/libs/scoreboard/CameraDecoder.h"
#include "hstream/src/libs/media/Mp4VideoTrack.h"
#include "hstream/src/libs/media/TestMp4.h"
#include "hstream/src/libs/scoreboard/CameraImages.h"
#include "hstream/src/libs/scoreboard/CameraSession.h"

#include <gst/rtsp-server/rtsp-server.h>
#include <iostream>
#include <thread>

namespace {
using namespace hm::scoreboard;
using hm::media::test::Check;
template <typename T>
T Take(absl::StatusOr<T> result) {
  Check(result.ok(), result.status().ToString());
  return std::move(*result);
}
void Status(const absl::Status& status) {
  Check(status.ok(), status.ToString());
}
void Video(const std::filesystem::path& path, bool hevc, bool fractional = false, unsigned integer_rate = 25) {
  std::vector<std::string> args{
      "-f",
      "lavfi",
      "-i",
      fractional ? "testsrc2=s=384x216:r=60000/1001:d=1"
                 : "testsrc2=s=384x216:r=" + std::to_string(integer_rate) + ":d=1",
      "-f",
      "lavfi",
      "-i",
      "anullsrc=r=48000:d=1.2",
      "-map",
      "0:v",
      "-map",
      "1:a",
      "-c:v",
      hevc ? "libx265" : "libx264",
      "-preset",
      "ultrafast",
      "-pix_fmt",
      hevc ? "yuv420p10le" : "yuv420p",
      "-bf",
      "2",
      "-color_range",
      "tv",
      "-colorspace",
      "bt709",
      "-color_primaries",
      "bt709",
      "-color_trc",
      "bt709",
      "-c:a",
      "aac",
      "-movflags",
      "+faststart+negative_cts_offsets"};
  if (hevc)
    args.insert(
        args.end(),
        {"-x265-params",
         "pools=1:frame-threads=1:log-level=error:colorprim=bt709:transfer=bt709:colormatrix=bt709:range=limited"});
  else
    args.insert(args.end(), {"-x264-params", "colorprim=bt709:transfer=bt709:colormatrix=bt709:fullrange=off"});
  args.push_back(path.string());
  hm::media::test::Ffmpeg(std::move(args));
}
void ReadFrames(CameraDecoder& decoder, unsigned first, bool cropped) {
  for (unsigned index = first; index < 25; ++index) {
    auto frame = Take(decoder.Read());
    Check(frame.has_value(), "scoreboard camera ended before all source frames arrived");
    Check(
        frame->time == index * 40 * GST_MSECOND && frame->duration == 40 * GST_MSECOND,
        "scoreboard frame phase/duration changed: expected=" + std::to_string(index * 40 * GST_MSECOND) +
            " actual=" + std::to_string(frame->time));
    Check(frame->cropped == cropped, "scoreboard crop coordinate space mislabeled");
    Check(
        frame->info.width == (cropped ? 128 : 384) && frame->info.height == (cropped ? 64 : 216),
        "scoreboard crop geometry changed");
  }
  Check(!Take(decoder.Read()).has_value(), "audio tail introduced an extra scoreboard frame");
}
std::vector<uint16_t> Pixels(const hm::gpu::Image& image) {
  std::vector<uint16_t> pixels(static_cast<size_t>(image.width) * image.height * 4);
  Check(
      cudaMemcpy2D(
          pixels.data(),
          image.width * 8,
          image.plane0,
          image.pitch0,
          image.width * 8,
          image.height,
          cudaMemcpyDeviceToHost) == cudaSuccess,
      "Cannot read test scoreboard pixels");
  return pixels;
}
void CheckCrop(const std::filesystem::path& path, CameraDecoderConfig config, GstClockTime start = 0) {
  const auto region = config.region;
  auto cropped = Take(CameraDecoder::Create(config));
  auto crop_pool = Take(CameraImages::Create(config.gpu_id, region.width, region.height, 2));
  config.region = {0, 0, config.source_width, config.source_height, {}};
  auto full = Take(CameraDecoder::Create(config));
  auto full_pool = Take(CameraImages::Create(config.gpu_id, config.source_width, config.source_height, 1));
  Status(cropped->OpenFile(path, start));
  Status(full->OpenFile(path, start));
  std::shared_ptr<const hm::gpu::Image> retained;
  std::vector<uint16_t> retained_pixels;
  for (unsigned index = 0; index < 25; ++index) {
    auto small_frame = Take(cropped->Read());
    auto full_frame = Take(full->Read());
    Check(small_frame && full_frame && small_frame->time == full_frame->time, "Crop changed frame timing");
    auto small_image = Take(crop_pool->Copy(*small_frame, region));
    auto full_image = Take(full_pool->Copy(*full_frame, config.region));
    const auto actual = Pixels(*small_image);
    const auto reference = Pixels(Take(CropCameraImage(*full_image, region)));
    Check(actual == reference, "Decoder crop changed source pixels");
    if (!index) {
      retained = small_image;
      retained_pixels = actual;
    } else {
      Check(
          absl::IsResourceExhausted(crop_pool->Copy(*small_frame, region).status()),
          "Camera pool overwrote a retained image instead of applying backpressure");
      Check(Pixels(*retained) == retained_pixels, "Retained scoreboard changed while decoder advanced");
    }
  }
  crop_pool.reset();
  Check(Pixels(*retained) == retained_pixels, "Pool shutdown invalidated a retained image");
}
// Exercise shutdown while the converter's bounded output pool is exhausted.
// End-of-stream-only tests never reach this state with the decoder still active.
void CheckPartialRead(const std::filesystem::path& path, CameraDecoderConfig config) {
  config.region = {0, 0, config.source_width, config.source_height, {}};
  auto decoder = Take(CameraDecoder::Create(config));
  Status(decoder->OpenFile(path, 0));
  Check(Take(decoder->Read()).has_value(), "Partial-read fixture has no frame");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  Status(decoder->Seek(400 * GST_MSECOND));
  auto frame = Take(decoder->Read());
  Check(frame && frame->time >= 400 * GST_MSECOND, "Queued pre-seek frame escaped after seek");
  frame.reset();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  Status(decoder->OpenFile(path, 0));
  frame = Take(decoder->Read());
  Check(frame && frame->time == 0, "Reopening retained an old queued frame");
  frame.reset();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto begin = std::chrono::steady_clock::now();
  decoder.reset();
  Check(
      std::chrono::steady_clock::now() - begin < std::chrono::seconds(5),
      "Partial-read decoder shutdown exceeded its deadline");
}
void CheckSession(const std::filesystem::path& path, const CameraDecoderConfig& decoder) {
  CameraSessionConfig config{
      decoder,
      {{path, 0, GST_SECOND, 0}, {path, GST_SECOND, GST_SECOND, 0}},
      80 * GST_MSECOND,
      80 * GST_MSECOND,
      1,
      {},
      nullptr,
      GST_CLOCK_TIME_NONE,
      0,
      false};
  auto session = Take(CameraSession::Start(config));
  std::vector<uint16_t> previous;
  for (int index = 0; index < 100; ++index) {
    const auto image = Take(session->Select((80 + index * 20) * GST_MSECOND));
    const auto pixels = Pixels(*image);
    if (index % 2)
      Check(pixels == previous, "50fps output selected different frames within a 25fps camera interval");
    previous = pixels;
  }
  Check(!session->Select(2080 * GST_MSECOND).ok(), "Scoreboard EOS reused an expired image");
  session.reset();
  config.epoch = 2;
  config.game_start_ns = 480 * GST_MSECOND;
  auto seeked = Take(CameraSession::Start(config));
  auto first = Take(seeked->Select(config.game_start_ns));
  const auto pixels = Pixels(*first);
  seeked->Cancel();
  Check(absl::IsCancelled(seeked->Select(config.game_start_ns).status()), "Stopped session still published output");
  seeked.reset();
  Check(Pixels(*first) == pixels, "Session shutdown freed an in-flight compositor image");
  // Stop while decode-ahead is blocked by the bounded queue must join the worker.
  auto stopped = Take(CameraSession::Start(config));
  Take(stopped->Select(config.game_start_ns));
  stopped->Cancel();
  stopped.reset();

  auto reference_decoder = Take(CameraDecoder::Create(decoder));
  Status(reference_decoder->OpenFile(path, 0));
  auto images = Take(CameraImages::Create(decoder.gpu_id, decoder.region.width, decoder.region.height, 1));
  std::vector<std::vector<uint16_t>> reference;
  for (unsigned index = 0; index < 25; ++index) {
    auto frame = Take(reference_decoder->Read());
    Check(frame.has_value(), "Mixed-rate reference ended early");
    reference.push_back(Pixels(*Take(images->Copy(*frame, decoder.region))));
  }
  reference_decoder.reset();
  images.reset();
  config.game_start_ns = config.offset_ns;
  for (unsigned output_fps : {10u, 30u, 60u}) {
    ++config.epoch;
    auto mixed = Take(CameraSession::Start(config));
    for (unsigned index = 0; index < output_fps * 2; ++index) {
      const auto time = gst_util_uint64_scale(index, GST_SECOND, output_fps);
      Check(
          Pixels(*Take(mixed->Select(config.offset_ns + time))) == reference[(index * 25 / output_fps) % 25],
          "Mixed-rate output did not repeat/skip the correct timestamp-matched scoreboard frame");
    }
  }
}
void CheckFractionalSession(
    const std::filesystem::path& path,
    const CameraDecoderConfig& config,
    bool hevc,
    bool prepared = false) {
  if (!prepared)
    Video(path, hevc, !hevc, 30);
  auto decoder = Take(CameraDecoder::Create(config));
  Status(decoder->OpenFile(path, 0));
  auto images = Take(CameraImages::Create(config.gpu_id, config.region.width, config.region.height, 1));
  std::vector<int64_t> times;
  std::vector<std::vector<uint16_t>> pixels;
  for (;;) {
    auto frame = Take(decoder->Read());
    if (!frame)
      break;
    Check(
        frame->time == gst_util_uint64_scale(times.size(), uint64_t(hevc ? 1 : 1001) * GST_SECOND, hevc ? 30 : 60000),
        "MP4 composition/segment rounding changed the original presentation timestamp");
    times.push_back(frame->time);
    pixels.push_back(Pixels(*Take(images->Copy(*frame, config.region))));
  }
  decoder.reset();
  images.reset();
  const auto track = Take(hm::media::ReadMp4VideoTrack(path, 5 * GST_SECOND, {}));
  CameraSessionConfig session_config{};
  session_config.decoder = config;
  session_config.chapters = {{path, 0, track.duration_ns, 0}, {path, track.duration_ns, track.duration_ns, 0}};
  session_config.epoch = 1;
  auto session = Take(CameraSession::Start(session_config));
  Check(times.size() == (hevc ? 30u : 60u), "Fractional-rate fixture lost frames");
  for (int chapter = 0; chapter < 2; ++chapter) {
    const auto start = chapter * track.duration_ns;
    for (size_t index = 0; index < times.size(); ++index) {
      Check(
          Pixels(*Take(session->Select(start + times[index]))) == pixels[index],
          "Fractional-rate session selected the wrong frame at its PTS");
      const auto end = index + 1 < times.size() ? times[index + 1] : track.duration_ns;
      Check(
          Pixels(*Take(session->Select(start + end - 1))) == pixels[index],
          "Fractional-rate interval has a gap or changed frame before the next PTS");
    }
  }
}
void CheckLive(CameraDecoderConfig config, const std::filesystem::path& recording) {
  auto* context = g_main_context_new();
  auto* loop = g_main_loop_new(context, FALSE);
  auto* server = gst_rtsp_server_new();
  gst_rtsp_server_set_address(server, "127.0.0.1");
  gst_rtsp_server_set_service(server, "0");
  auto* mounts = gst_rtsp_server_get_mount_points(server);
  auto* factory = gst_rtsp_media_factory_new();
  auto* escaped = g_strescape(recording.c_str(), nullptr);
  const std::string launch = std::string("( filesrc location=\"") + escaped +
      "\" ! qtdemux name=demux demux.video_0 ! queue ! h264parse ! identity sync=true "
      "! rtph264pay name=pay0 pt=96 )";
  g_free(escaped);
  gst_rtsp_media_factory_set_launch(factory, launch.c_str());
  gst_rtsp_mount_points_add_factory(mounts, "/scoreboard", factory);
  gst_object_unref(mounts);
  GError* error = nullptr;
  auto* source = gst_rtsp_server_create_source(server, nullptr, &error);
  Check(source != nullptr, error ? error->message : "Cannot start local RTSP fixture");
  g_clear_error(&error);
  g_source_attach(source, context);
  std::thread serving([&] { g_main_loop_run(loop); });
  auto* clock = gst_system_clock_obtain();
  const auto base = gst_clock_get_time(clock) - GST_SECOND;
  const auto cleanup = [&] {
    g_main_loop_quit(loop);
    serving.join();
    g_source_destroy(source);
    g_source_unref(source);
    gst_object_unref(server);
    gst_object_unref(clock);
    g_main_loop_unref(loop);
    g_main_context_unref(context);
  };
  try {
    config.live = true;
    config.codec = CameraCodec::kH264;
    auto decoder = Take(CameraDecoder::Create(config));
    Status(decoder->OpenLive(
        "rtsp://127.0.0.1:" + std::to_string(gst_rtsp_server_get_bound_port(server)) + "/scoreboard",
        clock,
        base,
        200,
        false));
    GstClockTime previous = GST_CLOCK_TIME_NONE;
    for (unsigned index = 0; index < 12; ++index) {
      auto frame = Take(decoder->Read());
      Check(frame.has_value(), "Live camera ended early");
      Check(frame->time >= GST_SECOND, "Live camera lost the shared game clock origin");
      Check(
          !GST_CLOCK_TIME_IS_VALID(frame->duration) || frame->duration == 40 * GST_MSECOND,
          "Live camera duration changed");
      if (GST_CLOCK_TIME_IS_VALID(previous))
        Check(frame->time > previous, "Live camera reordered presentation timestamps");
      previous = frame->time;
    }
    decoder->Cancel();
    Check(absl::IsCancelled(decoder->Read().status()), "Live decoder ignored cancellation");
    decoder.reset();
    const auto start = gst_clock_get_time(clock) + 250 * GST_MSECOND;
    CameraSessionConfig live{
        config,
        {},
        0,
        static_cast<int64_t>(start),
        7,
        "rtsp://127.0.0.1:" + std::to_string(gst_rtsp_server_get_bound_port(server)) + "/scoreboard",
        clock,
        gst_clock_get_time(clock),
        200,
        false};
    auto session = Take(CameraSession::Start(live));
    for (unsigned index = 0; index < 30; ++index)
      Take(session->Select(start + index * 20 * GST_MSECOND));
    session->Cancel();
    session.reset();
  } catch (...) {
    cleanup();
    throw;
  }
  cleanup();
}
} // namespace
int main(int argc, char** argv) {
  try {
    gst_init(&argc, &argv);
    std::filesystem::path large;
    if (argc >= 3 && std::string(argv[argc - 2]) == "--large-recording") {
      large = argv[argc - 1];
      argc -= 2;
    }
    const bool va = argc >= 3 && std::string(argv[1]) == "--va";
    const int next = va ? 3 : 1;
    const bool prepared = argc == next + 2 && std::string(argv[next]) == "--fixtures";
    Check(argc == next || prepared, "Usage: camera_decoder_test [--va NODE] [--fixtures DIRECTORY]");
    hm::media::test::Directory directory;
    for (const bool hevc : {false, true}) {
      const auto path =
          (prepared ? std::filesystem::path(argv[next + 1]) : directory.path) / (hevc ? "main10.mp4" : "h264.mp4");
      std::cerr << "Fixture " << path << std::endl;
      if (!prepared)
        Video(path, hevc);
      CameraDecoderConfig config{
          va ? CameraDecodeBackend::kVa : CameraDecodeBackend::kNvidia,
          hevc ? CameraCodec::kHevc : CameraCodec::kH264,
          false,
          0,
          va ? argv[2] : "",
          384,
          216,
          {96, 40, 128, 64, {}},
          2,
          2,
          5 * GST_SECOND,
          10 * GST_MSECOND};
      auto decoder = Take(CameraDecoder::Create(config));
      std::cerr << "Open" << std::endl;
      Status(decoder->OpenFile(path, 0));
      std::cerr << "Read" << std::endl;
      ReadFrames(*decoder, 0, !va);
      std::cerr << "Seek" << std::endl;
      Status(decoder->Seek(400 * GST_MSECOND));
      ReadFrames(*decoder, 10, !va);
      // Repeated opens model chapter rollover; no camera frames or audio tail
      // may leak from the retired chapter, even when the filename is reused.
      std::cerr << "Open" << std::endl;
      Status(decoder->OpenFile(path, 0));
      std::cerr << "Read" << std::endl;
      ReadFrames(*decoder, 0, !va);
      decoder->Cancel();
      Check(absl::IsCancelled(decoder->Read().status()), "decoder ignored cancellation");
      std::cerr << "Mismatch" << std::endl;
      std::cerr << "Partial read" << std::endl;
      CheckPartialRead(path, config);
      std::cerr << "Crop" << std::endl;
      CheckCrop(path, config);
      std::cerr << "Session" << std::endl;
      CheckSession(path, config);
      std::cerr << "Fractional rate" << std::endl;
      CheckFractionalSession(
          ((prepared && hevc) ? std::filesystem::path(argv[next + 1]) : directory.path) /
              (hevc ? "hevc-clock.mp4" : "fractional.mp4"),
          config,
          hevc,
          prepared && hevc);
      if (!hevc) {
        std::cerr << "Live" << std::endl;
        CheckLive(config, path);
      }
      config.source_width += 2;
      auto changed = Take(CameraDecoder::Create(config));
      Check(!changed->OpenFile(path, 0).ok(), "changed source geometry silently reused scoreboard coordinates");
    }
    if (!large.empty()) {
      auto track = Take(hm::media::ReadMp4VideoTrack(large, 5 * GST_SECOND, {}));
      CameraDecoderConfig config{
          va ? CameraDecodeBackend::kVa : CameraDecodeBackend::kNvidia,
          track.codec == hm::media::VideoCodec::kHevc ? CameraCodec::kHevc : CameraCodec::kH264,
          false,
          0,
          va ? argv[2] : "",
          track.width,
          track.height,
          {96, 40, 128, 64, {}},
          2,
          2,
          10 * GST_SECOND,
          10 * GST_MSECOND};
      CheckPartialRead(large, config);
      for (auto start : {0, 2, 10})
        CheckCrop(large, config, start * GST_SECOND);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
