#include "hstream/src/libs/gpu/vaapi/VaVideoPlaylist.h"

#include <sys/stat.h>

#include <limits>
#include <string>

namespace hm::gpu {
namespace {
bool SameFile(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
      a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
      a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
absl::StatusOr<GstClockTime> FrameTime(const media::Mp4VideoTrack& video, uint64_t frame) {
  const int64_t step_n = int64_t(video.fps_d) * video.time_base_d;
  const int64_t step_d = int64_t(video.fps_n) * video.time_base_n;
  if (step_n < step_d || step_n % step_d)
    return absl::FailedPreconditionError("Frame-index seek requires an integral frame duration on the MP4 clock");
  const auto ticks = static_cast<__int128>(frame) * (step_n / step_d) + video.start_ticks;
  if (ticks > INT64_MAX)
    return absl::DataLossError("Chapter frame timestamp exceeds the MP4 clock");
  const auto position = ticks * video.time_base_n * GST_SECOND / video.time_base_d;
  if (position < video.start_ns || position > INT64_MAX)
    return absl::DataLossError("Chapter frame timestamp exceeds its clock");
  return static_cast<GstClockTime>(position);
}
} // namespace
struct VaVideoPlaylist::Impl {
  struct Chapter {
    std::filesystem::path file;
    media::Mp4VideoTrack video;
    struct stat identity;
    GstClockTime start;
  };
  std::vector<Chapter> chapters;
  std::unique_ptr<VaVideoReader> reader;
  GstClockTime duration{}, previous{GST_CLOCK_TIME_NONE};
  uint64_t frames{};
  std::optional<uint64_t> expected_frame;
  bool allow_seek_prefix{false};
  bool constant_rate{true};
  size_t chapter{};
  bool positioned{false}, opened{false}, eos{false}, retired{false};
  std::atomic<bool> cancelled{false};
  absl::Status Check() const {
    if (cancelled.load(std::memory_order_relaxed))
      return absl::CancelledError("VA playlist cancelled");
    if (retired)
      return absl::FailedPreconditionError("VA playlist retired after failure");
    return absl::OkStatus();
  }
  absl::Status Retire(absl::Status status) {
    retired = true;
    return status;
  }
  absl::Status Open(size_t index, GstClockTime position) {
    const auto& target = chapters[index];
    struct stat current {};
    if (stat(target.file.c_str(), &current) || !SameFile(target.identity, current))
      return Retire(absl::FailedPreconditionError("Playlist chapter changed after discovery: " + target.file.string()));
    const auto status = opened && chapter == index ? reader->Seek(position) : reader->OpenFile(target.file, position);
    if (!status.ok())
      return Retire(status);
    chapter = index;
    opened = true;
    return absl::OkStatus();
  }
};
VaVideoPlaylist::VaVideoPlaylist(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VaVideoPlaylist::~VaVideoPlaylist() = default;

absl::StatusOr<std::unique_ptr<VaVideoPlaylist>> VaVideoPlaylist::Create(
    const VaVideoReaderConfig& config,
    const std::vector<std::filesystem::path>& files,
    const std::function<bool()>& is_cancelled) {
  if (files.empty() || !config.operation_timeout || config.operation_timeout > uint64_t(INT64_MAX))
    return absl::InvalidArgumentError("VA playlist requires chapters and a finite operation timeout");
  auto impl = std::make_unique<Impl>();
  for (const auto& file : files) {
    struct stat before {
    }, after{};
    if (stat(file.c_str(), &before) || !S_ISREG(before.st_mode))
      return absl::NotFoundError("Playlist chapter is not a regular file: " + file.string());
    auto info = media::ReadMp4VideoTrack(file, config.operation_timeout, is_cancelled);
    if (!info.ok())
      return info.status();
    if (stat(file.c_str(), &after) || !SameFile(before, after))
      return absl::FailedPreconditionError("Playlist chapter changed during discovery: " + file.string());
    if ((info->codec == media::VideoCodec::kHevc) != (config.codec == VaVideoCodec::kHevc))
      return absl::InvalidArgumentError("Playlist codec differs from the configured decoder");
    if (!impl->chapters.empty()) {
      const auto& first = impl->chapters.front().video;
      if (info->width != first.width || info->height != first.height ||
          int64_t(info->fps_n) * first.fps_d != int64_t(first.fps_n) * info->fps_d)
        return absl::InvalidArgumentError("Playlist chapters differ in video geometry or frame rate");
    }
    if (uint64_t(info->duration_ns) > uint64_t(INT64_MAX) - impl->duration)
      return absl::OutOfRangeError("Playlist duration overflows its clock");
    if (info->frames > UINT64_MAX - impl->frames)
      return absl::OutOfRangeError("Playlist frame count overflows");
    impl->chapters.push_back({file, *info, after, impl->duration});
    impl->duration += info->duration_ns;
    impl->frames += info->frames;
    impl->constant_rate &= info->constant_rate;
  }
  auto reader = VaVideoReader::Create(config);
  if (!reader.ok())
    return reader.status();
  impl->reader = std::move(*reader);
  return std::unique_ptr<VaVideoPlaylist>(new VaVideoPlaylist(std::move(impl)));
}

absl::Status VaVideoPlaylist::Seek(GstClockTime time) {
  auto& impl = *impl_;
  const auto status = impl.Check();
  if (!status.ok())
    return status;
  if (time > impl.duration)
    return absl::OutOfRangeError("Seek exceeds the playlist video duration");
  if (time == impl.duration) {
    impl.positioned = impl.eos = true;
    impl.previous = GST_CLOCK_TIME_NONE;
    impl.expected_frame.reset();
    return absl::OkStatus();
  }
  size_t index = 0;
  while (index + 1 < impl.chapters.size() && impl.chapters[index + 1].start <= time)
    ++index;
  const auto& chapter = impl.chapters[index];
  const auto opened = impl.Open(index, time - chapter.start + chapter.video.start_ns);
  if (!opened.ok())
    return opened;
  impl.positioned = true;
  impl.eos = false;
  impl.previous = GST_CLOCK_TIME_NONE;
  impl.expected_frame.reset();
  return absl::OkStatus();
}

absl::Status VaVideoPlaylist::SeekFrame(uint64_t frame) {
  auto& impl = *impl_;
  const auto status = impl.Check();
  if (!status.ok())
    return status;
  if (!impl.constant_rate)
    return absl::FailedPreconditionError("Frame-index seeks require constant-rate camera chapters");
  if (frame > impl.frames)
    return absl::OutOfRangeError("Seek exceeds the playlist frame count");
  if (frame == impl.frames)
    return Seek(impl.duration);
  size_t chapter = 0;
  while (frame >= impl.chapters[chapter].video.frames) {
    frame -= impl.chapters[chapter].video.frames;
    ++chapter;
  }
  const auto& video = impl.chapters[chapter].video;
  const auto position = FrameTime(video, frame);
  if (!position.ok())
    return position.status();
  if (*position - video.start_ns >= uint64_t(video.duration_ns))
    return impl.Retire(absl::DataLossError("Chapter frame grid differs from its duration"));
  const auto opened = impl.Open(chapter, *position);
  if (!opened.ok())
    return opened;
  impl.positioned = true;
  impl.eos = false;
  impl.previous = GST_CLOCK_TIME_NONE;
  impl.expected_frame = frame;
  impl.allow_seek_prefix = frame > 0;
  return absl::OkStatus();
}

absl::StatusOr<std::optional<VaVideoFrame>> VaVideoPlaylist::Read() {
  auto& impl = *impl_;
  for (;;) {
    const auto status = impl.Check();
    if (!status.ok())
      return status;
    if (!impl.positioned)
      return absl::FailedPreconditionError("Seek the playlist before reading");
    if (impl.eos)
      return std::nullopt;
    auto read = impl.reader->Read();
    if (!read.ok())
      return impl.Retire(read.status());
    if (!read->has_value()) {
      if (impl.expected_frame && *impl.expected_frame != impl.chapters[impl.chapter].video.frames)
        return impl.Retire(absl::DataLossError("Chapter ended before its advertised frame count"));
      if (impl.chapter + 1 == impl.chapters.size()) {
        impl.eos = true;
        return std::nullopt;
      }
      const auto next = impl.chapter + 1;
      const auto opened = impl.Open(next, impl.chapters[next].video.start_ns);
      if (!opened.ok())
        return opened;
      if (impl.expected_frame)
        impl.expected_frame = 0;
      impl.allow_seek_prefix = false;
      continue;
    }
    auto frame = std::move(**read);
    const auto& chapter = impl.chapters[impl.chapter];
    if (impl.expected_frame) {
      const auto index = *impl.expected_frame;
      if (index >= chapter.video.frames)
        return impl.Retire(absl::DataLossError("Chapter exceeded its advertised frame count"));
      const auto expected = FrameTime(chapter.video, index), next = FrameTime(chapter.video, index + 1);
      if (!expected.ok())
        return impl.Retire(expected.status());
      if (!next.ok())
        return impl.Retire(next.status());
      const auto duration = *next - *expected;
      const auto time_error = frame.time > *expected ? frame.time - *expected : *expected - frame.time;
      if (impl.allow_seek_prefix) {
        impl.allow_seek_prefix = false;
        // qtdemux's B-frame segment translation can carry one nanosecond
        // differently from the MP4 clock. Discard at most one clipped tail of
        // the preceding picture; never expose it as the requested frame.
        if (time_error <= 1 && frame.duration <= 1 && duration > 2)
          continue;
      }
      // Demuxers floor absolute timestamps and frame durations independently,
      // so a correct duration can differ from the interval by one nanosecond.
      // Header rate agreement alone does not prove a constant presentation grid.
      if (time_error > 1 || !GST_CLOCK_TIME_IS_VALID(frame.duration) ||
          (frame.duration > duration ? frame.duration - duration : duration - frame.duration) > 1)
        return impl.Retire(absl::DataLossError(
            "Decoded chapter violates its constant frame grid at frame " + std::to_string(index) +
            ": time=" + std::to_string(frame.time) + " expected=" + std::to_string(*expected) +
            " duration=" + std::to_string(frame.duration) + " expected-duration=" + std::to_string(duration)));
      ++*impl.expected_frame;
    }
    if (frame.time < uint64_t(chapter.video.start_ns) ||
        frame.time - chapter.video.start_ns >= uint64_t(chapter.video.duration_ns) ||
        frame.info.width != chapter.video.width || frame.info.height != chapter.video.height ||
        int64_t(frame.info.fps_n) * chapter.video.fps_d != int64_t(chapter.video.fps_n) * frame.info.fps_d)
      return impl.Retire(absl::DataLossError("Decoded chapter differs from its video metadata"));
    frame.time = chapter.start + frame.time - chapter.video.start_ns;
    if (GST_CLOCK_TIME_IS_VALID(impl.previous) && frame.time <= impl.previous)
      return impl.Retire(absl::DataLossError("Playlist video time did not increase"));
    impl.previous = frame.time;
    return std::optional<VaVideoFrame>(std::move(frame));
  }
}
void VaVideoPlaylist::Cancel() noexcept {
  impl_->cancelled.store(true, std::memory_order_relaxed);
  impl_->reader->Cancel();
}
GstClockTime VaVideoPlaylist::duration() const {
  return impl_->duration;
}
uint64_t VaVideoPlaylist::frame_count() const {
  return impl_->frames;
}
const media::Mp4VideoTrack& VaVideoPlaylist::video_info() const {
  return impl_->chapters.front().video;
}
} // namespace hm::gpu
