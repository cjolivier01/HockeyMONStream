#include "hstream/src/libs/media/Mp4VideoTrack.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include <chrono>
#include <limits>
#include <memory>
#include <string>

namespace hm::media {
namespace {
using Clock = std::chrono::steady_clock;
struct Interrupt {
  Clock::time_point deadline;
  const std::function<bool()>& cancelled;
  bool did_cancel{false}, expired{false}, callback_failed{false};
  int Poll() noexcept {
    try {
      did_cancel |= cancelled && cancelled();
    } catch (...) {
      callback_failed = true;
    }
    expired |= Clock::now() >= deadline;
    return did_cancel || expired || callback_failed;
  }
  absl::Status Status() const {
    if (callback_failed)
      return absl::InternalError("MP4 cancellation callback threw an exception");
    if (did_cancel)
      return absl::CancelledError("MP4 metadata discovery cancelled");
    if (expired)
      return absl::DeadlineExceededError("MP4 metadata discovery timed out");
    return absl::OkStatus();
  }
};
absl::StatusOr<int64_t> Nanoseconds(int64_t ticks, AVRational base) {
  if (ticks < 0 || base.num <= 0 || base.den <= 0 ||
      static_cast<long double>(ticks) * base.num * 1000000000 / base.den >= std::numeric_limits<int64_t>::max())
    return absl::DataLossError("MP4 video timing is negative, absent or overflows nanoseconds");
  return av_rescale_q(ticks, base, {1, 1000000000});
}
} // namespace

absl::StatusOr<Mp4VideoTrack> ReadMp4VideoTrack(
    const std::filesystem::path& file,
    int64_t timeout_ns,
    const std::function<bool()>& is_cancelled) {
  if (timeout_ns <= 0)
    return absl::InvalidArgumentError("MP4 metadata requires a positive timeout");
  const auto now = Clock::now();
  const auto duration = std::chrono::nanoseconds(timeout_ns);
  Interrupt interrupt{
      duration > Clock::time_point::max() - now ? Clock::time_point::max() : now + duration, is_cancelled};
  if (interrupt.Poll())
    return interrupt.Status();
  std::error_code error;
  if (!std::filesystem::is_regular_file(file, error) || error)
    return absl::NotFoundError("MP4 source is not a local regular file: " + file.string());
  // Fail before dereferencing public structures with an incompatible ABI.
  if ((avformat_version() >> 16) != LIBAVFORMAT_VERSION_MAJOR ||
      (avcodec_version() >> 16) != LIBAVCODEC_VERSION_MAJOR || (avutil_version() >> 16) != LIBAVUTIL_VERSION_MAJOR)
    return absl::FailedPreconditionError("MP4 metadata requires matching FFmpeg headers and libraries");
  const auto* demuxer = av_find_input_format("mov");
  if (!demuxer)
    return absl::FailedPreconditionError("FFmpeg MP4 demuxer is unavailable");
  AVFormatContext* context = avformat_alloc_context();
  if (!context)
    return absl::ResourceExhaustedError("Cannot allocate MP4 metadata demuxer");
  context->interrupt_callback = {[](void* p) { return static_cast<Interrupt*>(p)->Poll(); }, &interrupt};
  AVDictionary* options = nullptr;
  av_dict_set(&options, "protocol_whitelist", "file", 0);
  av_dict_set(&options, "enable_drefs", "0", 0);
  const int opened = avformat_open_input(&context, file.c_str(), demuxer, &options);
  av_dict_free(&options);
  const auto close = [](AVFormatContext* p) { avformat_close_input(&p); };
  std::unique_ptr<AVFormatContext, decltype(close)> owner(context, close);
  if (interrupt.Poll())
    return interrupt.Status();
  if (opened < 0) {
    char detail[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(opened, detail, sizeof(detail));
    return absl::DataLossError("Cannot read MP4 metadata: " + file.string() + ": " + detail);
  }
  const AVStream* video = nullptr;
  for (unsigned index = 0; index < context->nb_streams; ++index) {
    const auto* stream = context->streams[index];
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
      continue;
    if (video || (stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
      return absl::InvalidArgumentError("MP4 requires one unambiguous video track");
    video = stream;
  }
  if (!video)
    return absl::InvalidArgumentError("MP4 has no video track");
  const auto* codec = video->codecpar;
  if (codec->codec_id != AV_CODEC_ID_H264 && codec->codec_id != AV_CODEC_ID_HEVC)
    return absl::UnimplementedError("Program MP4 input requires H.264 or HEVC");
  if (codec->width <= 0 || codec->height <= 0 || video->avg_frame_rate.num <= 0 || video->avg_frame_rate.den <= 0 ||
      video->nb_frames <= 0 || video->duration <= 0 || video->duration == AV_NOPTS_VALUE ||
      video->start_time == AV_NOPTS_VALUE)
    return absl::DataLossError("MP4 requires finite video-track geometry, frame count and timing");
  auto start = Nanoseconds(video->start_time, video->time_base);
  auto length = Nanoseconds(video->duration, video->time_base);
  if (!start.ok())
    return start.status();
  if (!length.ok())
    return length.status();
  if (*length <= 0 || *start > std::numeric_limits<int64_t>::max() - *length)
    return absl::DataLossError("MP4 video duration is empty or overflows its clock");
  return Mp4VideoTrack{
      codec->codec_id == AV_CODEC_ID_H264 ? VideoCodec::kH264 : VideoCodec::kHevc,
      codec->width,
      codec->height,
      video->avg_frame_rate.num,
      video->avg_frame_rate.den,
      static_cast<uint64_t>(video->nb_frames),
      *start,
      *length};
}
} // namespace hm::media
