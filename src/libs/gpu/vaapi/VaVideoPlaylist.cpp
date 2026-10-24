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
    impl->chapters.push_back({file, *info, after, impl->duration});
    impl->duration += info->duration_ns;
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
      if (impl.chapter + 1 == impl.chapters.size()) {
        impl.eos = true;
        return std::nullopt;
      }
      const auto next = impl.chapter + 1;
      const auto opened = impl.Open(next, impl.chapters[next].video.start_ns);
      if (!opened.ok())
        return opened;
      continue;
    }
    auto frame = std::move(**read);
    const auto& chapter = impl.chapters[impl.chapter];
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
const media::Mp4VideoTrack& VaVideoPlaylist::video_info() const {
  return impl_->chapters.front().video;
}
} // namespace hm::gpu
