#include "hstream/src/libs/media/Mp4AudioReader.h"
#include "hstream/src/libs/media/TestMp4.h"

#include <cmath>
#include <map>

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
void Fixture(const std::filesystem::path& path, const char* offset) {
  test::Ffmpeg(
      {"-f",
       "lavfi",
       "-i",
       "color=s=384x192:r=25:d=1",
       "-f",
       "lavfi",
       "-i",
       "sine=frequency=997:sample_rate=48000:duration=1.2",
       "-c:v",
       "libx264",
       "-bf",
       "2",
       "-c:a",
       "aac",
       "-aac_pns",
       "0",
       "-ac",
       "2",
       "-output_ts_offset",
       offset,
       path.string()});
}
void Run() {
  test::Directory directory;
  const auto path = directory.path / "audio.mp4";
  Fixture(path, "0");
  auto reader = Take(Mp4AudioReader::Create(bounds));
  Check(!reader->Read().ok(), "Unpositioned read succeeded");
  Ok(reader->Open(path));
  Check(reader->format().rate == 48000 && reader->format().channels == 2, "Wrong audio format");
  Ok(reader->Seek(0));
  std::map<int64_t, float> reference;
  auto retained = Take(reader->Read());
  Check(retained.has_value() && retained->sample == 0, "AAC priming escaped at file origin");
  double energy = 0;
  auto frame = retained;
  for (;;) {
    if (!frame)
      break;
    for (size_t i = 0; i < frame->samples(); ++i) {
      reference.emplace(frame->sample + i, frame->pcm[i * 2]);
      energy += std::abs(frame->pcm[i * 2]);
    }
    frame = Take(reader->Read());
  }
  Check(reference.size() >= 57600 && reference.size() <= 58624 && energy > 100, "Missing decoded audio");
  for (const int64_t position : {24000, 24001, 1024, 1, 47999, 0}) {
    Ok(reader->Seek(position));
    const auto seek = Take(reader->Read());
    Check(
        seek.has_value() && seek->sample <= position && seek->sample + int64_t(seek->samples()) > position,
        "Seek did not return the frame containing the requested sample");
    for (size_t i = 0; i < seek->samples(); ++i) {
      const auto sample = seek->sample + i;
      if (int64_t(sample) < position)
        continue;
      // Perceptual noise substitution has decoder-local random state; permit
      // its low-level variation while detecting a one-sample phase error.
      Check(
          std::abs(reference.at(sample) - seek->pcm[i * 2]) < 0.0001f,
          "Seek changed PCM after the target sample " + std::to_string(sample) +
              " difference=" + std::to_string(std::abs(reference.at(sample) - seek->pcm[i * 2])));
    }
  }
  reader.reset();
  Check(retained->samples() > 0 && retained->pcm[10] == retained->pcm[10], "Retained PCM lost ownership");

  const auto shifted = directory.path / "shifted.mp4";
  Fixture(shifted, "0.3");
  reader = Take(Mp4AudioReader::Create(bounds));
  Ok(reader->Open(shifted));
  Ok(reader->Seek(24000));
  frame = Take(reader->Read());
  Check(
      frame.has_value() && frame->sample <= 24000 && frame->sample + int64_t(frame->samples()) > 24000,
      "Shifted file seek ignored edits");
  reader->Cancel();
  Check(absl::IsCancelled(reader->Read().status()), "Read ignored cancellation");
  Check(absl::IsCancelled(reader->Seek(0)), "Seek ignored cancellation");

  reader = Take(Mp4AudioReader::Create({bounds.operation_timeout_ns, 1, bounds.maximum_frame_samples}));
  Ok(reader->Open(path));
  Ok(reader->Seek(0));
  Check(absl::IsResourceExhausted(reader->Read().status()), "Packet bound ignored");
  Check(absl::IsFailedPrecondition(reader->Seek(0)), "Failed reader was revived");
  reader = Take(Mp4AudioReader::Create({bounds.operation_timeout_ns, bounds.maximum_packet_bytes, 1}));
  Ok(reader->Open(path));
  Ok(reader->Seek(0));
  Check(!reader->Read().ok(), "Frame sample bound ignored");
  Check(!Mp4AudioReader::Create({0, 1, 1}).ok(), "Zero timeout accepted");
  Check(!Mp4AudioReader::Create({1, 0, 1}).ok(), "Zero packet bound accepted");
  std::cout << "MP4 audio seek, priming, ownership, bounds and cancellation passed\n";
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
