#include "hstream/src/libs/media/AudioPlaylist.h"
#include "hstream/src/libs/media/TestMp4.h"

#include <cmath>

namespace {
using namespace hm::media;
using test::Check;
template <class T>
T Take(absl::StatusOr<T> result) {
  Check(result.ok(), result.status().ToString());
  return std::move(*result);
}
void Ok(absl::Status status) {
  Check(status.ok(), status.ToString());
}
const Mp4AudioReaderConfig bounds{5'000'000'000, 1024 * 1024, 4096};
void Tone(const std::filesystem::path& path) {
  test::Ffmpeg(
      {"-f",
       "lavfi",
       "-i",
       "color=s=384x192:r=25:d=0.2",
       "-f",
       "lavfi",
       "-i",
       "sine=frequency=997:sample_rate=48000:duration=0.5",
       "-c:v",
       "libx264",
       "-bf",
       "2",
       "-c:a",
       "aac",
       "-ac",
       "2",
       path.string()});
}
void Run() {
  test::Directory directory;
  const auto first = directory.path / "first.mp4", second = directory.path / "second.mp4";
  Tone(first);
  test::Chapter(second, "red", "0.3");
  auto playlist = Take(AudioPlaylist::Create(bounds, {first, second}));
  Check(playlist->duration_ns() == 400000000, "Audio tails changed chapter clock");
  Check(!playlist->ReadUntil(1).ok(), "Unpositioned playlist read succeeded");
  Ok(playlist->Seek(0));
  size_t samples = 0;
  double energy = 0;
  for (int64_t end : {17000000, 90000000, 200000001, 400000000}) {
    for (;;) {
      auto frame = Take(playlist->ReadUntil(end));
      if (!frame)
        break;
      Check(
          frame->sample == int64_t(samples) && frame->samples() <= 4096,
          "Audio timeline is unbounded or discontinuous");
      for (size_t i = 0; i < frame->samples(); ++i) {
        if (samples + i >= 9600)
          Check(std::abs(frame->pcm[i * 2]) < 0.00001f, "First chapter audio tail leaked into second chapter");
        else
          energy += std::abs(frame->pcm[i * 2]);
      }
      samples += frame->samples();
    }
    Check(samples == size_t((end * 48000 + 500000000) / 1000000000), "Audio watermark rounded inconsistently");
  }
  Check(samples == 19200 && energy > 100, "Chapter samples lost");
  Check(!Take(playlist->ReadUntil(500000000)), "Audio exceeded video EOS");
  Check(!playlist->ReadUntil(499999999).ok(), "Backward watermark was accepted");
  Ok(playlist->Seek(199990000));
  auto frame = Take(playlist->ReadUntil(400000000));
  Check(frame && frame->sample == 9600, "Boundary seek did not round once to the audio clock");
  Ok(playlist->Seek(80000000));
  frame = Take(playlist->ReadUntil(100000000));
  Check(frame && frame->sample == 3840 && frame->samples() == 960, "Backward seek failed");
  auto retained = std::move(frame);
  playlist->Cancel();
  Check(absl::IsCancelled(playlist->ReadUntil(200000000).status()), "Cancellation ignored");
  playlist.reset();
  Check(retained->pcm.size() == 1920, "Returned samples were borrowed");

  playlist = Take(AudioPlaylist::Create(bounds, {first, second}));
  std::filesystem::rename(second, directory.path / "old.mp4");
  test::Chapter(second, "blue", "0");
  Check(!playlist->Seek(200000000).ok(), "Replaced chapter accepted");
  Check(!playlist->Seek(0).ok(), "Retired playlist revived");
  const auto no_audio = directory.path / "silent.mp4";
  test::Ffmpeg({"-f", "lavfi", "-i", "color=s=384x192:r=25:d=0.2", "-c:v", "libx264", no_audio.string()});
  Check(!AudioPlaylist::Create(bounds, {no_audio}).ok(), "Missing audio track silently accepted");
  const auto gap = directory.path / "gap.mp4";
  test::Ffmpeg(
      {"-f",
       "lavfi",
       "-i",
       "color=s=384x192:r=25:d=0.2",
       "-itsoffset",
       "0.08",
       "-f",
       "lavfi",
       "-i",
       "sine=frequency=997:sample_rate=48000:duration=0.06",
       "-c:v",
       "libx264",
       "-c:a",
       "aac",
       "-ac",
       "2",
       gap.string()});
  playlist = Take(AudioPlaylist::Create(bounds, {gap}));
  Ok(playlist->Seek(0));
  energy = 0;
  samples = 0;
  for (;;) {
    frame = Take(playlist->ReadUntil(200000000));
    if (!frame)
      break;
    for (size_t i = 0; i < frame->samples(); ++i) {
      const auto index = frame->sample + i;
      if (index < 1920 || index >= 8640)
        Check(std::abs(frame->pcm[i * 2]) < 0.00001f, "Audio gaps were not filled with silence");
      if (index >= 4800 && index < 5760)
        energy += std::abs(frame->pcm[i * 2]);
    }
    samples += frame->samples();
  }
  Check(samples == 9600 && energy > 10, "Gap handling lost audio or shortened the video clock");
  std::cout << "Audio chapter clock, watermarks, tails, seeks and identity passed\n";
}
} // namespace
int main() {
  try {
    Run();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
