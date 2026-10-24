#include "hstream/src/libs/gpu/vaapi/VaVideoPlaylist.h"

#include <fstream>

#include "hstream/src/libs/gpu/vaapi/VaHipFrame.h"
#include "hstream/src/libs/media/StereoFramePlan.h"
#include "hstream/src/libs/media/TestMp4.h"

namespace {
using hm::media::test::Check;
void Status(const absl::Status& status) {
  Check(status.ok(), status.ToString());
}
template <class T>
T Take(absl::StatusOr<T> result) {
  Status(result.status());
  return std::move(*result);
}
void Pixel(const hm::gpu::VaVideoFrame& frame, bool white) {
  hipStream_t stream{};
  Check(hipStreamCreate(&stream) == hipSuccess, "Cannot create test stream");
  {
    auto input = Take(hm::gpu::VaHipFrame::Import(
        gst_sample_get_buffer(frame.sample.get()), stream, hm::gpu::VaHipFrame::Access::kRead));
    uint8_t luma = 0;
    // Only one synthetic test luma byte is read back to check retained ownership.
    Check(
        hipMemcpyAsync(&luma, input->plane(0).data, 1, hipMemcpyDeviceToHost, stream) == hipSuccess,
        "Cannot inspect synthetic chapter");
    Status(input->Complete());
    Check(white ? luma >= 234 : luma <= 17, "Retained chapter changed pixels");
  }
  Check(hipStreamDestroy(stream) == hipSuccess, "Cannot destroy test stream");
}
hm::gpu::VaVideoFrame Frame(hm::gpu::VaVideoPlaylist& playlist, unsigned index) {
  std::cout << "Reading playlist frame " << index << std::endl;
  auto frame = Take(playlist.Read());
  Check(frame.has_value(), "Unexpected playlist EOS");
  Check(frame->time == index * 40 * GST_MSECOND, "Audio tail or chapter origin shifted video time");
  return std::move(*frame);
}
} // namespace
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  try {
    hm::media::test::Directory root;
    const auto black = root.path / "black.mp4", white = root.path / "white.mp4", offset = root.path / "offset.mp4";
    hm::media::test::Chapter(black, "black", "0");
    hm::media::test::Chapter(white, "white", "0");
    hm::media::test::Chapter(offset, "black", "0.4");
    char pci[32]{};
    Check(hipDeviceGetPCIBusId(pci, sizeof(pci), 0) == hipSuccess, "Cannot select AMD test device");
    hm::gpu::VaVideoReaderConfig config{
        hm::gpu::VaVideoCodec::kH264,
        GST_VIDEO_FORMAT_NV12,
        std::string("/dev/dri/by-path/pci-") + pci + "-render",
        2,
        10 * GST_SECOND,
        10 * GST_MSECOND};
    auto playlist = Take(hm::gpu::VaVideoPlaylist::Create(config, {black, white, offset}, {}));
    Check(playlist->duration() == 600 * GST_MSECOND, "Playlist used container durations");
    Check(!playlist->Read().ok(), "Unpositioned playlist read accepted");
    std::cout << "Initial seek" << std::endl;
    Status(playlist->Seek(0));
    auto retained = Frame(*playlist, 0);
    for (unsigned i = 1; i < 15; ++i)
      Pixel(Frame(*playlist, i), i >= 5 && i < 10);
    Check(!Take(playlist->Read()).has_value(), "Playlist missing EOS");
    std::cout << "Chapter-boundary seek" << std::endl;
    Status(playlist->Seek(200 * GST_MSECOND));
    Pixel(Frame(*playlist, 5), true);
    Status(playlist->Seek(440 * GST_MSECOND));
    Pixel(Frame(*playlist, 11), false);
    Status(playlist->Seek(playlist->duration()));
    Check(!Take(playlist->Read()).has_value(), "Seek to end did not yield EOS");
    Check(absl::IsOutOfRange(playlist->Seek(GST_CLOCK_TIME_NONE)), "Out-of-range seek accepted");
    Status(playlist->Seek(40 * GST_MSECOND));
    Pixel(Frame(*playlist, 1), false);
    const auto descriptors = [] {
      size_t count = 0;
      for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd"))
        (void)entry, ++count;
      return count;
    };
    const auto before = descriptors();
    for (unsigned index = 0; index < 12; ++index) {
      Status(playlist->Seek((index % 3) * 200 * GST_MSECOND));
      Pixel(Frame(*playlist, (index % 3) * 5), index % 3 == 1);
    }
    Check(descriptors() <= before + 4, "Repeated audio/video chapters leaked descriptors");
    {
      auto partner = Take(hm::gpu::VaVideoPlaylist::Create(config, {black, white, offset}, {}));
      for (const double frame_offset : {.25, .5, .75}) {
        for (const double start : {0., .001, .19}) {
          const auto plan = Take(hm::media::PlanStereoFrames(start, frame_offset, 0, 25, 1));
          Status(playlist->SeekFrame(plan.left_frame));
          Status(partner->SeekFrame(plan.right_frame));
          for (unsigned step = 0; step < 3; ++step) {
            const auto left = Take(playlist->Read()), right = Take(partner->Read());
            Check(
                left && right && left->time == plan.left_time_ns + step * 40 * GST_MSECOND &&
                    right->time == plan.right_time_ns + step * 40 * GST_MSECOND && left->duration == 40 * GST_MSECOND &&
                    right->duration == 40 * GST_MSECOND,
                "Fractional synchronization clipped first-frame timestamps or drifted across chapters");
          }
        }
      }
    }
    playlist->Cancel();
    Check(
        absl::IsCancelled(playlist->Read().status()) && absl::IsCancelled(playlist->Seek(0)),
        "Playlist cancellation was not sticky");
    playlist.reset();
    Pixel(retained, false);
    {
      const auto chapter = [&](const char* name, const char* color, const char* origin = "0") {
        const auto path = root.path / name;
        hm::media::test::Ffmpeg(
            {"-f",
             "lavfi",
             "-i",
             std::string("color=c=") + color + ":s=384x192:r=30",
             "-frames:v",
             "5",
             "-c:v",
             "libx264",
             "-bf",
             "2",
             "-pix_fmt",
             "yuv420p",
             "-colorspace",
             "bt709",
             "-color_primaries",
             "bt709",
             "-color_trc",
             "bt709",
             "-output_ts_offset",
             origin,
             "-movie_timescale",
             "15360",
             path.string()});
        return path;
      };
      const auto a = chapter("thirty-a.mp4", "black"), b = chapter("thirty-b.mp4", "black"),
                 c = chapter("thirty-c.mp4", "white");
      auto thirty = Take(hm::gpu::VaVideoPlaylist::Create(config, {a, b, c}, {}));
      Check(thirty->frame_count() == 15, "Playlist frame count changed");
      const auto plan = Take(hm::media::PlanStereoFrames(1. / 3, 0, 0, 30, 1));
      Check(plan.left_frame == 10, "30fps chapter-boundary plan selected the wrong frame");
      Status(thirty->SeekFrame(plan.left_frame));
      auto selected = Take(thirty->Read());
      Check(
          selected && selected->time == 2 * uint64_t(thirty->video_info().duration_ns) &&
              selected->duration >= 33 * GST_MSECOND,
          "Rounded chapter boundary returned the old frame with a clipped timestamp");
      Pixel(*selected, true);
      unsigned count = 1;
      while (Take(thirty->Read()))
        ++count;
      Check(count == 5, "Frame-index chapter seek returned an extra/missing frame");
      Status(thirty->SeekFrame(5));
      selected = Take(thirty->Read());
      Check(selected && selected->time == uint64_t(thirty->video_info().duration_ns), "Second chapter seek shifted");
      Pixel(*selected, false);
      Status(thirty->SeekFrame(15));
      Check(!Take(thirty->Read()), "Frame-index seek to end did not yield EOS");
      Check(absl::IsOutOfRange(thirty->SeekFrame(16)), "Past-end frame index accepted");
      const auto shifted = chapter("thirty-offset.mp4", "white", "0.033333333");
      auto fractional = Take(hm::gpu::VaVideoPlaylist::Create(config, {shifted}, {}));
      Check(
          fractional->video_info().start_ticks == 512 && fractional->video_info().time_base_d == 15360,
          "Fractional origin fixture lost its exact MP4 clock");
      Status(fractional->SeekFrame(2));
      selected = Take(fractional->Read());
      Check(
          selected && selected->time == 66666667 && selected->duration >= 33 * GST_MSECOND,
          "Separately rounded frame offset and origin returned a clipped previous frame");
      Pixel(*selected, true);
      count = 1;
      while (Take(fractional->Read()))
        ++count;
      Check(count == 3, "Fractional-origin frame seek returned an extra/missing frame");
      for (unsigned index = 0; index < 5; ++index) {
        for (auto* source : {thirty.get(), fractional.get()}) {
          Status(source->SeekFrame(index));
          selected = Take(source->Read());
          Check(
              selected && selected->duration >= 33 * GST_MSECOND, "B-frame segment rounding exposed a tiny old frame");
          count = 1;
          while (Take(source->Read()))
            ++count;
          Check(count == source->frame_count() - index, "Frame seek changed the remaining picture count");
        }
      }
      const auto variable = root.path / "variable-presentation.mp4";
      hm::media::test::Ffmpeg(
          {"-f",
           "lavfi",
           "-i",
           "testsrc2=s=384x192:r=30",
           "-frames:v",
           "30",
           "-vf",
           "settb=1/30000,setpts=N*1000+if(eq(N\\,3)\\,150\\,0)",
           "-fps_mode",
           "vfr",
           "-enc_time_base",
           "1:30000",
           "-c:v",
           "libx264",
           "-bf",
           "2",
           variable.string()});
      auto irregular = Take(hm::gpu::VaVideoPlaylist::Create(config, {variable}, {}));
      Check(irregular->video_info().constant_rate, "Fixture must exercise misleading constant-rate headers");
      Status(irregular->SeekFrame(3));
      Check(
          absl::IsDataLoss(irregular->Read().status()), "Irregular presentation timestamps accepted as a stereo frame");
      Check(!irregular->SeekFrame(0).ok(), "Invalid frame timeline did not retire the playlist");
      irregular = Take(hm::gpu::VaVideoPlaylist::Create(config, {variable}, {}));
      Status(irregular->SeekFrame(4));
      Check(absl::IsDataLoss(irregular->Read().status()), "Clipped variable-rate frame passed a PTS-only check");
    }
    playlist = Take(hm::gpu::VaVideoPlaylist::Create(config, {black, white}, {}));
    // Only our synthetic file is changed. Stale prepared metadata must fail.
    std::ofstream(white, std::ios::app) << 'x';
    Check(absl::IsFailedPrecondition(playlist->Seek(200 * GST_MSECOND)), "Replaced chapter accepted");
    Check(!playlist->Seek(0).ok(), "Failed playlist was revived");
    Check(!hm::gpu::VaVideoPlaylist::Create(config, {}, {}).ok(), "Empty playlist accepted");
    const auto audio = root.path / "only-audio.mp4";
    hm::media::test::Ffmpeg({"-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo:d=0.1", "-c:a", "aac", audio.string()});
    auto video_reader = Take(hm::gpu::VaVideoReader::Create(config));
    Check(absl::IsUnavailable(video_reader->OpenFile(audio, 0)), "Missing selected video did not fail explicitly");
    config.codec = hm::gpu::VaVideoCodec::kHevc;
    Check(!hm::gpu::VaVideoPlaylist::Create(config, {black}, {}).ok(), "Mixed decoder codec accepted");
    std::cout << "VA chapter video clock, seeks, retained samples and cancellation passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
