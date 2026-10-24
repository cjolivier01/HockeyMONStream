#include "hstream/src/libs/gpu/vaapi/VaStereoVideo.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "hstream/src/libs/media/StereoFramePlan.h"

namespace hm::gpu {
struct VaStereoVideo::Impl {
  std::array<std::unique_ptr<VaVideoPlaylist>, 2> readers;
  std::array<double, 2> offsets;
  std::array<uint64_t, 2> next{}, end{};
  GstClockTime duration{};
  uint64_t sequence{};
  bool positioned{false}, eos{false}, retired{false};
  std::atomic<bool> cancelled{false};
  absl::Status Check() const {
    if (cancelled.load(std::memory_order_relaxed))
      return absl::CancelledError("Stereo video cancelled");
    if (retired)
      return absl::FailedPreconditionError("Stereo video retired after failure");
    return absl::OkStatus();
  }
  absl::Status Retire(absl::Status status) {
    retired = true;
    for (auto& reader : readers)
      reader->Cancel();
    return status;
  }
  absl::StatusOr<GstClockTime> Time(uint64_t left_frame) const {
    const auto& video = readers[0]->video_info();
    long double frames = static_cast<long double>(left_frame) - offsets[0];
    // The frame planner snaps values within the original double's uncertainty
    // at integer boundaries. Preserve that same tolerance at synchronized zero.
    const long double error =
        2 * std::numeric_limits<double>::epsilon() * (static_cast<long double>(left_frame) + offsets[0]);
    if (frames < 0 && frames >= -error)
      frames = 0;
    const long double time = frames * video.fps_d * GST_SECOND / video.fps_n;
    if (!std::isfinite(time) || time < 0 || time > INT64_MAX)
      return absl::OutOfRangeError("Synchronized video time exceeds its clock");
    return static_cast<GstClockTime>(time);
  }
};

VaStereoVideo::VaStereoVideo(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VaStereoVideo::~VaStereoVideo() = default;
absl::StatusOr<std::unique_ptr<VaStereoVideo>> VaStereoVideo::Create(
    const VaStereoVideoConfig& config,
    const std::function<bool()>& is_cancelled) {
  auto impl = std::make_unique<Impl>();
  impl->offsets = config.frame_offsets;
  for (size_t camera = 0; camera < 2; ++camera) {
    auto reader = VaVideoPlaylist::Create(config.readers[camera], config.chapters[camera], is_cancelled);
    if (!reader.ok())
      return reader.status();
    impl->readers[camera] = std::move(*reader);
  }
  const auto& left = impl->readers[0]->video_info();
  const auto& right = impl->readers[1]->video_info();
  if (int64_t(left.fps_n) * right.fps_d != int64_t(right.fps_n) * left.fps_d ||
      int64_t(left.fps_d) * GST_SECOND < left.fps_n)
    return absl::InvalidArgumentError("Stereo cameras require equal frame rates on a nanosecond clock");
  const auto start = media::PlanStereoFrames(0, impl->offsets[0], impl->offsets[1], left.fps_n, left.fps_d);
  if (!start.ok())
    return start.status();
  const std::array<uint64_t, 2> begin{start->left_frame, start->right_frame};
  for (size_t camera = 0; camera < 2; ++camera)
    if (begin[camera] >= impl->readers[camera]->frame_count())
      return absl::InvalidArgumentError("Camera synchronization leaves no paired frames");
  const auto count = std::min(impl->readers[0]->frame_count() - begin[0], impl->readers[1]->frame_count() - begin[1]);
  for (size_t camera = 0; camera < 2; ++camera)
    impl->end[camera] = begin[camera] + count;
  const auto duration = impl->Time(impl->end[0]);
  if (!duration.ok())
    return duration.status();
  impl->duration = *duration;
  return std::unique_ptr<VaStereoVideo>(new VaStereoVideo(std::move(impl)));
}

absl::Status VaStereoVideo::Seek(GstClockTime time) {
  auto& impl = *impl_;
  const auto check = impl.Check();
  if (!check.ok())
    return check;
  if (time > impl.duration)
    return absl::OutOfRangeError("Seek exceeds the synchronized video duration");
  if (time == impl.duration) {
    impl.positioned = impl.eos = true;
    impl.sequence = 0;
    return absl::OkStatus();
  }
  const auto& video = impl.readers[0]->video_info();
  const auto plan =
      media::PlanStereoFrames(double(time) / GST_SECOND, impl.offsets[0], impl.offsets[1], video.fps_n, video.fps_d);
  if (!plan.ok())
    return plan.status();
  const std::array<uint64_t, 2> next{plan->left_frame, plan->right_frame};
  // A sub-frame tail has no next complete pair, so it selects clean EOS.
  if (next[0] >= impl.end[0] || next[1] >= impl.end[1]) {
    impl.positioned = impl.eos = true;
    impl.sequence = 0;
    return absl::OkStatus();
  }
  for (size_t camera = 0; camera < 2; ++camera) {
    const auto seek = impl.readers[camera]->SeekFrame(next[camera]);
    if (!seek.ok())
      return impl.Retire(seek);
  }
  impl.next = next;
  impl.sequence = 0;
  impl.positioned = true;
  impl.eos = false;
  return absl::OkStatus();
}

absl::StatusOr<std::optional<VaStereoFrame>> VaStereoVideo::Read() {
  auto& impl = *impl_;
  const auto check = impl.Check();
  if (!check.ok())
    return check;
  if (!impl.positioned)
    return absl::FailedPreconditionError("Seek stereo video before reading");
  if (impl.eos)
    return std::nullopt;
  if (impl.next[0] == impl.end[0]) {
    // Verify actual EOS on whichever camera(s) set the paired end. The longer
    // camera may retain frames beyond that end, which are intentionally unused.
    for (size_t camera = 0; camera < 2; ++camera) {
      if (impl.next[camera] != impl.readers[camera]->frame_count())
        continue;
      const auto extra = impl.readers[camera]->Read();
      if (!extra.ok())
        return impl.Retire(extra.status());
      if (*extra)
        return impl.Retire(absl::DataLossError("Stereo camera exceeded its prepared frame count"));
    }
    impl.eos = true;
    return std::nullopt;
  }
  VaStereoFrame pair;
  for (size_t camera = 0; camera < 2; ++camera) {
    auto frame = impl.readers[camera]->Read();
    if (!frame.ok())
      return impl.Retire(frame.status());
    if (!*frame)
      return impl.Retire(absl::DataLossError("Stereo camera ended before its prepared frame count"));
    pair.cameras[camera] = std::move(**frame);
  }
  const auto time = impl.Time(impl.next[0]), next_time = impl.Time(impl.next[0] + 1);
  if (!time.ok())
    return impl.Retire(time.status());
  if (!next_time.ok())
    return impl.Retire(next_time.status());
  pair.source_indices = impl.next;
  pair.sequence = impl.sequence++;
  pair.time = *time;
  pair.duration = *next_time - *time;
  for (auto& next : impl.next)
    ++next;
  return std::optional<VaStereoFrame>(std::move(pair));
}
void VaStereoVideo::Cancel() noexcept {
  impl_->cancelled.store(true, std::memory_order_relaxed);
  for (auto& reader : impl_->readers)
    reader->Cancel();
}
GstClockTime VaStereoVideo::duration() const {
  return impl_->duration;
}
const media::Mp4VideoTrack& VaStereoVideo::video_info(size_t camera) const {
  return impl_->readers.at(camera)->video_info();
}
} // namespace hm::gpu
