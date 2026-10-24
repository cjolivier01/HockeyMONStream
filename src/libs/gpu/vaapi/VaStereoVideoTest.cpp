#include "hstream/src/libs/gpu/vaapi/VaStereoVideo.h"

#include <cmath>
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
    uint8_t luma{};
    // One synthetic byte checks which source picture survived the pair/seek.
    Check(
        hipMemcpyAsync(&luma, input->plane(0).data, 1, hipMemcpyDeviceToHost, stream) == hipSuccess,
        "Cannot inspect test picture");
    Status(input->Complete());
    Check(white ? luma >= 234 : luma <= 17, "Paired frame has the wrong chapter pixels");
  }
  Check(hipStreamDestroy(stream) == hipSuccess, "Cannot destroy test stream");
}
} // namespace
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  try {
    hm::media::test::Directory root;
    const auto black = root.path / "black.mp4", white = root.path / "white.mp4", shifted = root.path / "shifted.mp4";
    hm::media::test::Chapter(black, "black", "0");
    hm::media::test::Chapter(white, "white", "0");
    hm::media::test::Chapter(shifted, "black", "0.4");
    char pci[32]{};
    Check(hipDeviceGetPCIBusId(pci, sizeof(pci), 0) == hipSuccess, "Cannot find AMD test GPU");
    const hm::gpu::VaVideoReaderConfig reader{
        hm::gpu::VaVideoCodec::kH264,
        GST_VIDEO_FORMAT_NV12,
        std::string("/dev/dri/by-path/pci-") + pci + "-render",
        2,
        10 * GST_SECOND,
        10 * GST_MSECOND};
    hm::gpu::VaStereoVideoConfig config{{reader, reader}, {{{black, white}, {black, white, shifted}}}, {.25, .75}};
    auto source = Take(hm::gpu::VaStereoVideo::Create(config, {}));
    Check(source->duration() == 390 * GST_MSECOND, "Paired duration included audio or unsynchronized tails");
    Check(!source->Read().ok(), "Unpositioned stereo read succeeded");
    Status(source->Seek(0));
    std::optional<hm::gpu::VaStereoFrame> retained;
    for (unsigned sequence = 0; sequence < 9; ++sequence) {
      auto pair = Take(source->Read());
      Check(
          pair && pair->sequence == sequence && pair->source_indices[0] == sequence + 1 &&
              pair->source_indices[1] == sequence + 2 && pair->time == (30 + sequence * 40) * GST_MSECOND &&
              pair->duration == 40 * GST_MSECOND,
          "Stereo pairing or synchronized clock drifted");
      Pixel(pair->cameras[0], sequence >= 4);
      Pixel(pair->cameras[1], sequence >= 3 && sequence < 8);
      if (!sequence)
        retained = std::move(pair);
    }
    Check(!Take(source->Read()), "Shorter synchronized camera did not end the source");
    Status(source->Seek(150 * GST_MSECOND));
    auto pair = Take(source->Read());
    Check(
        pair && pair->sequence == 0 && pair->source_indices[0] == 4 && pair->source_indices[1] == 5 &&
            pair->time == 150 * GST_MSECOND,
        "Stereo seek did not atomically reset pairing and sequence");
    Pixel(pair->cameras[0], false);
    Pixel(pair->cameras[1], true);
    Status(source->Seek(source->duration()));
    Check(!Take(source->Read()), "Seek to paired end did not select EOS");
    Check(absl::IsOutOfRange(source->Seek(GST_CLOCK_TIME_NONE)), "Invalid stereo seek accepted");
    Status(source->Seek(0));
    Check(Take(source->Read())->sequence == 0, "EOS prevented a backward stereo seek");
    source->Cancel();
    Check(absl::IsCancelled(source->Read().status()) && absl::IsCancelled(source->Seek(0)), "Cancellation not sticky");
    source.reset();
    Pixel(retained->cameras[0], false);
    Pixel(retained->cameras[1], false);
    Check(
        absl::IsCancelled(hm::gpu::VaStereoVideo::Create(config, [] { return true; }).status()),
        "Preparation cancellation ignored");
    auto bad = config;
    bad.frame_offsets[0] = 100;
    Check(!hm::gpu::VaStereoVideo::Create(bad, {}).ok(), "Empty synchronized overlap accepted");
    bad.frame_offsets[0] = -.1;
    Check(!hm::gpu::VaStereoVideo::Create(bad, {}).ok(), "Negative synchronization accepted");
    bad = config;
    bad.frame_offsets[0] = std::nextafter(1.0, 2.0);
    auto boundary = Take(hm::gpu::VaStereoVideo::Create(bad, {}));
    Status(boundary->Seek(0));
    pair = Take(boundary->Read());
    Check(
        pair && pair->source_indices[0] == 1 && pair->time == 0,
        "Frame-planner rounding produced negative synchronized time");
    const auto thirty = root.path / "thirty.mp4";
    hm::media::test::Ffmpeg(
        {"-f",
         "lavfi",
         "-i",
         "color=c=black:s=384x192:r=30",
         "-frames:v",
         "5",
         "-c:v",
         "libx264",
         "-bf",
         "2",
         "-pix_fmt",
         "yuv420p",
         thirty.string()});
    bad = config;
    bad.chapters = {{{thirty, thirty, thirty}, {thirty, thirty}}};
    auto rational = Take(hm::gpu::VaStereoVideo::Create(bad, {}));
    Status(rational->Seek(0));
    for (uint64_t index = 0; index < 8; ++index) {
      pair = Take(rational->Read());
      const auto expected = (index * 4 + 3) * GST_SECOND / 120;
      const auto next = (index * 4 + 7) * GST_SECOND / 120;
      Check(
          pair && pair->sequence == index && pair->time == expected && pair->duration == next - expected,
          "Chapter rounding leaked into the synchronized output clock");
    }
    Check(!Take(rational->Read()), "Shorter right camera did not end the pair");
    Check(rational->duration() == 875 * GST_SECOND / 3000, "Right-limited duration used a chapter clock");
    source = Take(hm::gpu::VaStereoVideo::Create(config, {}));
    Status(source->Seek(0));
    // Mutate only this test's second source chapter. A partial seek must retire
    // the pair, never combine a freshly sought left with an old right frame.
    std::ofstream(white, std::ios::app) << 'x';
    Check(!source->Seek(150 * GST_MSECOND).ok(), "Changed second-camera chapter accepted");
    Check(!source->Read().ok() && !source->Seek(0).ok(), "Half-failed stereo seek remained usable");
    std::cout << "Stereo pairing, source clock, retained frames, EOS, seek failure and cancellation passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
