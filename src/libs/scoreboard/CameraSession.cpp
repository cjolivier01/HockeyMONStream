#include "hstream/src/libs/scoreboard/CameraSession.h"
#include "hstream/src/libs/scoreboard/CameraClock.h"

#include <limits>
#include <thread>

namespace hm::scoreboard {
namespace {
bool Add(int64_t a, int64_t b, int64_t& out) {
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
    return false;
  out = a + b;
  return true;
}
} // namespace
struct CameraSession::Impl {
  CameraSessionConfig config;
  CameraFrames frames;
  std::unique_ptr<CameraDecoder> decoder;
  std::unique_ptr<CameraImages> images;
  std::thread worker;
  std::mutex mutex;
  absl::Status failure;
  std::condition_variable ready_changed;
  bool ready = false, finished = false, cancelled = false;
  explicit Impl(const CameraSessionConfig& value) : config(value), frames(value.decoder.queued_frames) {
    if (config.live_clock)
      gst_object_ref(config.live_clock);
  }
  ~Impl() {
    frames.Cancel();
    if (decoder)
      decoder->Cancel();
    if (worker.joinable())
      worker.join();
    if (config.live_clock)
      gst_object_unref(config.live_clock);
  }
  absl::Status Produce(const CameraChapter* chapter) {
    auto pending = decoder->Read();
    for (;;) {
      auto frame = std::move(pending);
      if (!frame.ok())
        return frame.status();
      if (!*frame)
        return absl::OkStatus();
      if ((*frame)->time > INT64_MAX)
        return absl::OutOfRangeError("Scoreboard camera timestamp exceeds signed timeline");
      int64_t time = (*frame)->time;
      if (chapter) {
        if (time < chapter->stream_origin_ns)
          return absl::DataLossError("Scoreboard frame precedes its video-track origin");
        time -= chapter->stream_origin_ns;
        if (time >= chapter->duration_ns)
          return absl::DataLossError("Scoreboard frame exceeds its video-track duration");
        if (!Add(time, chapter->start_ns, time))
          return absl::OutOfRangeError("Scoreboard chapter time overflow");
      }
      // Fractional-rate MP4 timestamps and durations round independently to
      // nanoseconds. Join adjacent intervals at the actual next PTS when the
      // difference is only rounding. Preserve larger recorded gaps/overlaps
      // for validation. Live clock correction always uses the next actual PTS.
      pending = decoder->Read();
      if (!pending.ok())
        return pending.status();
      GstClockTime duration = (*frame)->duration;
      if (*pending) {
        const auto delta = (*pending)->time - (*frame)->time;
        const auto difference = delta > duration ? delta - duration : duration - delta;
        if (config.decoder.live || difference <= 1)
          duration = delta;
      } else if (chapter) {
        const auto remaining = static_cast<GstClockTime>(chapter->start_ns + chapter->duration_ns - time);
        const auto difference = remaining > duration ? remaining - duration : duration - remaining;
        if (difference <= 1)
          duration = remaining;
      }
      if (duration > INT64_MAX)
        return absl::OutOfRangeError("Scoreboard presentation interval overflow");
      if (config.decoder.live && (config.live_base_time > INT64_MAX || !Add(time, config.live_base_time, time)))
        return absl::OutOfRangeError("Live scoreboard clock overflow");
      int64_t end;
      if (!Add(time, config.offset_ns, time) || !Add(time, duration, end))
        return absl::OutOfRangeError("Scoreboard synchronization offset overflow");
      // Decode preroll remains exact but does not enter the output interval.
      if (end <= config.game_start_ns)
        continue;
      time = std::max<int64_t>(0, time);
      auto image = images->Copy(**frame, config.decoder.region);
      if (!image.ok())
        return image.status();
      frame->reset(); // Release hardware surfaces before waiting on the compositor.
      const auto result = frames.WaitPublish({config.epoch, time, end, *image}, std::chrono::nanoseconds::max());
      if (result == CameraFrames::PublishResult::kCancelled || result == CameraFrames::PublishResult::kObsolete)
        return absl::CancelledError("Scoreboard camera session retired");
      if (result != CameraFrames::PublishResult::kPublished)
        return absl::DataLossError("Scoreboard presentation intervals overlap or cannot be queued");
      {
        std::lock_guard<std::mutex> lock(mutex);
        ready = true;
        ready_changed.notify_all();
      }
    }
  }
  absl::Status Run() {
    if (config.decoder.live) {
      auto status = decoder->OpenLive(
          config.live_uri, config.live_clock, config.live_base_time, config.live_latency_ms, config.live_ntp_sync);
      return status.ok() ? Produce(nullptr) : status;
    }
    int64_t source_start;
    if (config.offset_ns == INT64_MIN || !Add(config.game_start_ns, -config.offset_ns, source_start))
      return absl::OutOfRangeError("Scoreboard start offset overflow");
    for (const auto& chapter : config.chapters) {
      if (source_start >= chapter.start_ns + chapter.duration_ns)
        continue;
      const int64_t local = source_start <= chapter.start_ns ? 0 : source_start - chapter.start_ns;
      int64_t seek;
      if (!Add(chapter.stream_origin_ns, local, seek))
        return absl::OutOfRangeError("Scoreboard chapter seek overflow");
      auto status = decoder->OpenFile(chapter.path, seek);
      if (!status.ok())
        return status;
      status = Produce(&chapter);
      if (!status.ok())
        return status;
    }
    return absl::OkStatus();
  }
};
CameraSession::CameraSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CameraSession::~CameraSession() = default;
absl::StatusOr<std::unique_ptr<CameraSession>> CameraSession::Start(const CameraSessionConfig& config) {
  if (!config.epoch || config.game_start_ns < 0 || config.decoder.queued_frames > UINT_MAX - 3 ||
      (config.decoder.live ? !config.chapters.empty() : config.chapters.empty()))
    return absl::InvalidArgumentError("Scoreboard session requires an epoch and one source mode");
  if (config.decoder.live && (!GST_IS_CLOCK(config.live_clock) || !GST_CLOCK_TIME_IS_VALID(config.live_base_time)))
    return absl::InvalidArgumentError("Live scoreboard requires the game clock and base time");
  int64_t end = 0;
  for (const auto& chapter : config.chapters) {
    if (chapter.start_ns != end || chapter.duration_ns <= 0 || chapter.stream_origin_ns < 0 ||
        !Add(chapter.start_ns, chapter.duration_ns, end))
      return absl::InvalidArgumentError("Scoreboard chapters require contiguous video-track timing");
  }
  auto decoder = CameraDecoder::Create(config.decoder);
  if (!decoder.ok())
    return decoder.status();
  auto images = CameraImages::Create(
      config.decoder.gpu_id,
      config.decoder.region.width,
      config.decoder.region.height,
      config.decoder.queued_frames + 3);
  if (!images.ok())
    return images.status();
  auto impl = std::make_unique<Impl>(config);
  impl->decoder = std::move(*decoder);
  impl->images = std::move(*images);
  impl->frames.Reset(config.epoch);
  auto* state = impl.get();
  impl->worker = std::thread([state] {
    absl::Status status;
    try {
      status = state->Run();
    } catch (const std::exception& error) {
      status = absl::InternalError(error.what());
    } catch (...) {
      status = absl::InternalError("Unexpected scoreboard worker failure");
    }
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->failure = std::move(status);
    }
    state->frames.End(state->config.epoch);
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->finished = true;
      state->ready_changed.notify_all();
    }
  });
  return std::unique_ptr<CameraSession>(new CameraSession(std::move(impl)));
}
absl::StatusOr<std::shared_ptr<const gpu::Image>> CameraSession::Select(
    int64_t game_time_ns,
    const std::function<bool()>& cancelled) {
  using Clock = CameraClock;
  const auto now = Clock::now();
  const auto allowance = std::chrono::nanoseconds(impl_->config.decoder.operation_timeout);
  const auto deadline = allowance >= Clock::time_point::max() - now ? Clock::time_point::max() : now + allowance;
  CameraFrames::Selection selected;
  for (;;) {
    if (cancelled && cancelled()) {
      Cancel();
      return absl::CancelledError("Scoreboard camera session cancelled");
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now());
    const auto wait = std::min(remaining, std::chrono::nanoseconds(impl_->config.decoder.cancellation_poll_interval));
    selected = impl_->frames.WaitSelect(impl_->config.epoch, game_time_ns, wait);
    if (selected.status != CameraFrames::SelectionStatus::kTimeout || Clock::now() >= deadline)
      break;
  }
  if (selected.status == CameraFrames::SelectionStatus::kReady)
    return std::static_pointer_cast<const gpu::Image>(selected.frame->image);
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->failure.ok())
      return impl_->failure;
  }
  if (selected.status == CameraFrames::SelectionStatus::kCancelled)
    return absl::CancelledError("Scoreboard camera session cancelled");
  if (selected.status == CameraFrames::SelectionStatus::kTimeout)
    return absl::DeadlineExceededError("Matching scoreboard frame did not arrive");
  return absl::DataLossError("No scoreboard frame covers the requested output time");
}
absl::Status CameraSession::WaitReady() {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  const auto now = CameraClock::now();
  const auto allowance = std::chrono::nanoseconds(impl_->config.decoder.operation_timeout);
  const auto deadline =
      allowance >= CameraClock::time_point::max() - now ? CameraClock::time_point::max() : now + allowance;
  while (!impl_->ready && !impl_->finished && !impl_->cancelled) {
    const auto remaining = deadline - CameraClock::now();
    if (remaining <= std::chrono::nanoseconds::zero())
      return absl::DeadlineExceededError("Scoreboard camera did not establish its initial interval");
    impl_->ready_changed.wait_for(
        lock, std::min(remaining, std::chrono::nanoseconds(impl_->config.decoder.cancellation_poll_interval)));
  }
  if (impl_->cancelled)
    return absl::CancelledError("Scoreboard camera startup cancelled");
  if (!impl_->failure.ok())
    return impl_->failure;
  return impl_->ready ? absl::OkStatus() : absl::DataLossError("Scoreboard camera ended before its first interval");
}
void CameraSession::Cancel() {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->cancelled = true;
    impl_->ready_changed.notify_all();
  }
  impl_->frames.Cancel();
  impl_->decoder->Cancel();
}
} // namespace hm::scoreboard
