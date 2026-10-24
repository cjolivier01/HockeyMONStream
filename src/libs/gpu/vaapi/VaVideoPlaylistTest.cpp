#include "hstream/src/libs/gpu/vaapi/VaVideoPlaylist.h"

#include <fstream>

#include "hstream/src/libs/gpu/vaapi/VaHipFrame.h"
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
    playlist->Cancel();
    Check(
        absl::IsCancelled(playlist->Read().status()) && absl::IsCancelled(playlist->Seek(0)),
        "Playlist cancellation was not sticky");
    playlist.reset();
    Pixel(retained, false);
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
