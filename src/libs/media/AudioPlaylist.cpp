#include "hstream/src/libs/media/AudioPlaylist.h"

#include <sys/stat.h>

#include <algorithm>
#include <atomic>

#include "hstream/src/libs/media/Mp4VideoTrack.h"

namespace hm::media {
namespace {
int64_t Samples(int64_t ns, int rate) {
  return (static_cast<__int128>(ns) * rate + 500000000) / 1000000000;
}
bool SameFile(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
      a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
      a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
} // namespace
struct AudioPlaylist::Impl {
  struct Chapter {
    std::filesystem::path file;
    struct stat identity;
    int64_t start_ns, end_ns, origin_ns;
  };
  std::vector<Chapter> chapters;
  std::unique_ptr<Mp4AudioReader> reader;
  Mp4AudioReaderConfig config;
  AudioFormat format{};
  std::optional<AudioFrame> pending;
  std::atomic<bool> cancelled{false};
  int64_t cursor{}, duration{}, watermark{};
  size_t chapter{};
  bool positioned{false}, retired{false}, audio_eos{false};
  absl::Status Check() const {
    if (cancelled.load(std::memory_order_relaxed))
      return absl::CancelledError("Audio playlist cancelled");
    if (retired)
      return absl::FailedPreconditionError("Audio playlist retired after failure");
    return absl::OkStatus();
  }
  absl::Status Retire(absl::Status status) {
    retired = true;
    return status;
  }
  absl::Status Open(size_t index, int64_t position) {
    const auto& next = chapters[index];
    struct stat identity {};
    if (stat(next.file.c_str(), &identity) || !SameFile(next.identity, identity))
      return Retire(absl::FailedPreconditionError("Audio chapter changed after discovery: " + next.file.string()));
    auto status = reader->Open(next.file);
    if (!status.ok())
      return Retire(status);
    if (reader->format().rate != format.rate || reader->format().channels != format.channels)
      return Retire(absl::DataLossError("Audio chapter format changed after discovery"));
    const auto local = position - Samples(next.start_ns, format.rate) + Samples(next.origin_ns, format.rate);
    status = reader->Seek(local);
    if (!status.ok())
      return Retire(status);
    chapter = index;
    pending.reset();
    audio_eos = false;
    return absl::OkStatus();
  }
};
AudioPlaylist::AudioPlaylist(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AudioPlaylist::~AudioPlaylist() = default;
absl::StatusOr<std::unique_ptr<AudioPlaylist>> AudioPlaylist::Create(
    const Mp4AudioReaderConfig& config,
    const std::vector<std::filesystem::path>& files) {
  if (files.empty())
    return absl::InvalidArgumentError("Audio playlist requires camera chapters");
  auto impl = std::make_unique<Impl>();
  impl->config = config;
  auto reader = Mp4AudioReader::Create(config);
  if (!reader.ok())
    return reader.status();
  impl->reader = std::move(*reader);
  for (const auto& file : files) {
    struct stat before {
    }, after{};
    if (stat(file.c_str(), &before) || !S_ISREG(before.st_mode))
      return absl::NotFoundError("Audio chapter is not a regular file: " + file.string());
    const auto video = ReadMp4VideoTrack(file, config.operation_timeout_ns, {});
    if (!video.ok())
      return video.status();
    const auto status = impl->reader->Open(file);
    if (!status.ok())
      return status;
    if (stat(file.c_str(), &after) || !SameFile(before, after))
      return absl::FailedPreconditionError("Audio chapter changed during discovery: " + file.string());
    const auto format = impl->reader->format();
    if (!impl->chapters.empty() && (format.rate != impl->format.rate || format.channels != impl->format.channels))
      return absl::InvalidArgumentError("Audio chapters differ in sample rate or channel count");
    impl->format = format;
    if (impl->duration > INT64_MAX - video->duration_ns)
      return absl::OutOfRangeError("Audio playlist exceeds the video clock");
    const auto end = impl->duration + video->duration_ns;
    impl->chapters.push_back({file, after, impl->duration, end, video->start_ns});
    impl->duration = end;
  }
  return std::unique_ptr<AudioPlaylist>(new AudioPlaylist(std::move(impl)));
}
absl::Status AudioPlaylist::Seek(int64_t time_ns) {
  auto& impl = *impl_;
  auto status = impl.Check();
  if (!status.ok())
    return status;
  if (time_ns < 0 || time_ns > impl.duration)
    return absl::OutOfRangeError("Audio seek exceeds the video clock");
  const auto cursor = Samples(time_ns, impl.format.rate);
  size_t chapter = 0;
  while (chapter + 1 < impl.chapters.size() && cursor >= Samples(impl.chapters[chapter].end_ns, impl.format.rate))
    ++chapter;
  if (cursor < Samples(impl.duration, impl.format.rate)) {
    status = impl.Open(chapter, cursor);
    if (!status.ok())
      return status;
  }
  impl.cursor = cursor;
  impl.watermark = time_ns;
  impl.positioned = true;
  return absl::OkStatus();
}
absl::StatusOr<std::optional<AudioFrame>> AudioPlaylist::ReadUntil(int64_t end_ns) {
  auto& impl = *impl_;
  const auto status = impl.Check();
  if (!status.ok())
    return status;
  if (!impl.positioned)
    return absl::FailedPreconditionError("Seek audio playlist before reading");
  if (end_ns < impl.watermark)
    return absl::InvalidArgumentError("Audio watermark cannot move backwards without a seek");
  impl.watermark = end_ns;
  const auto end = Samples(std::min(end_ns, impl.duration), impl.format.rate);
  if (impl.cursor >= end)
    return std::nullopt;
  if (impl.cursor >= Samples(impl.chapters[impl.chapter].end_ns, impl.format.rate)) {
    const auto opened = impl.Open(impl.chapter + 1, impl.cursor);
    if (!opened.ok())
      return opened;
  }
  const auto& chapter = impl.chapters[impl.chapter];
  const auto count = std::min<int64_t>(
      impl.config.maximum_frame_samples, std::min(end, Samples(chapter.end_ns, impl.format.rate)) - impl.cursor);
  AudioFrame output{impl.cursor, impl.format, std::vector<float>(size_t(count) * impl.format.channels, 0)};
  const auto local_begin =
      impl.cursor - Samples(chapter.start_ns, impl.format.rate) + Samples(chapter.origin_ns, impl.format.rate);
  const auto local_end = local_begin + count;
  for (;;) {
    const auto checked = impl.Check();
    if (!checked.ok())
      return impl.Retire(checked);
    if (!impl.pending && !impl.audio_eos) {
      auto read = impl.reader->Read();
      if (!read.ok())
        return impl.Retire(read.status());
      impl.pending = std::move(*read);
      impl.audio_eos = !impl.pending;
    }
    if (!impl.pending || impl.pending->sample >= local_end)
      break;
    const auto& frame = *impl.pending;
    const auto from = std::max(local_begin, frame.sample);
    const auto frame_end = frame.sample + int64_t(frame.samples());
    const auto to = std::min(local_end, frame_end);
    if (to > from)
      std::copy_n(
          frame.pcm.data() + (from - frame.sample) * impl.format.channels,
          (to - from) * impl.format.channels,
          output.pcm.data() + (from - local_begin) * impl.format.channels);
    if (frame_end > local_end)
      break;
    impl.pending.reset();
  }
  impl.cursor += count;
  return std::optional<AudioFrame>(std::move(output));
}
void AudioPlaylist::Cancel() noexcept {
  impl_->cancelled.store(true, std::memory_order_relaxed);
  impl_->reader->Cancel();
}
AudioFormat AudioPlaylist::format() const {
  return impl_->format;
}
int64_t AudioPlaylist::duration_ns() const {
  return impl_->duration;
}
} // namespace hm::media
