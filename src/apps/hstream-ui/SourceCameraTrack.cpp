#include "src/apps/hstream-ui/SourceCameraTrack.h"

#include <gst/gst.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/parseutils.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>

namespace hm::ui {
absl::StatusOr<SourceCameraTrack> ReadSourceCameraTrack(
    const QString& file,
    qint64 timeout_ms,
    const std::function<bool()>& cancelled) {
  if (timeout_ms <= 0 || timeout_ms > std::numeric_limits<qint64>::max() / GST_MSECOND)
    return absl::InvalidArgumentError("Camera metadata requires a bounded positive timeout");
  if (!file.endsWith(".mkv", Qt::CaseInsensitive) && !file.endsWith(".avi", Qt::CaseInsensitive)) {
    auto track = media::ReadMp4VideoTrack(file.toStdString(), timeout_ms * GST_MSECOND, cancelled);
    if (!track.ok())
      return track.status();
    return SourceCameraTrack{track->codec, track->width, track->height, track->start_ns, track->duration_ns, "qtdemux"};
  }
  // These containers do not share MP4's strict header timing contract. Inspect
  // headers and, if necessary, compressed timestamps only; never open a codec.
  using Clock = std::chrono::steady_clock;
  const auto now = Clock::now();
  const auto timeout = std::chrono::milliseconds(timeout_ms);
  auto deadline = timeout > Clock::time_point::max() - now ? Clock::time_point::max() : now + timeout;
  if ((avformat_version() >> 16) != LIBAVFORMAT_VERSION_MAJOR ||
      (avcodec_version() >> 16) != LIBAVCODEC_VERSION_MAJOR || (avutil_version() >> 16) != LIBAVUTIL_VERSION_MAJOR)
    return absl::FailedPreconditionError("Camera metadata requires matching FFmpeg headers and libraries");
  AVFormatContext* context = avformat_alloc_context();
  if (!context)
    return absl::ResourceExhaustedError("Cannot allocate camera metadata demuxer");
  struct Interrupt {
    Clock::time_point deadline;
    const std::function<bool()>& cancelled;
    bool stopped{false}, callback_failed{false};
    bool Poll() noexcept {
      try {
        stopped = stopped || (cancelled && cancelled());
      } catch (...) {
        callback_failed = true;
      }
      return stopped || callback_failed || Clock::now() >= deadline;
    }
    absl::Status Status() const {
      if (callback_failed)
        return absl::InternalError("Camera metadata cancellation callback threw an exception");
      if (stopped)
        return absl::CancelledError("Camera metadata cancelled");
      if (Clock::now() >= deadline)
        return absl::DeadlineExceededError("Camera metadata inspection timed out");
      return absl::OkStatus();
    }
  } interrupt{deadline, cancelled};
  context->interrupt_callback = {[](void* value) { return int(static_cast<Interrupt*>(value)->Poll()); }, &interrupt};
  if (interrupt.Poll()) {
    avformat_free_context(context);
    return interrupt.Status();
  }
  AVDictionary* options = nullptr;
  av_dict_set(&options, "protocol_whitelist", "file", 0);
  av_dict_set(&options, "format_whitelist", "matroska,webm,avi", 0);
  const int opened = avformat_open_input(&context, file.toLocal8Bit().constData(), nullptr, &options);
  av_dict_free(&options);
  const auto close = [](AVFormatContext* value) { avformat_close_input(&value); };
  std::unique_ptr<AVFormatContext, decltype(close)> owner(context, close);
  interrupt.Poll();
  if (!interrupt.Status().ok())
    return interrupt.Status();
  if (opened < 0 || Clock::now() >= deadline)
    return absl::DataLossError("Cannot read camera container metadata within its deadline");
  AVStream* video = nullptr;
  for (unsigned index = 0; index < context->nb_streams; ++index) {
    auto* stream = context->streams[index];
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
      continue;
    if (video || (stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
      return absl::InvalidArgumentError("Camera preview requires one unambiguous video track");
    video = stream;
  }
  if (!video || video->codecpar->width <= 0 || video->codecpar->height <= 0 || video->time_base.num <= 0 ||
      video->time_base.den <= 0)
    return absl::DataLossError("Camera recording has no usable video track");
  const auto* codec = video->codecpar;
  if (codec->codec_id != AV_CODEC_ID_H264 && codec->codec_id != AV_CODEC_ID_HEVC)
    return absl::UnimplementedError("Camera hardware preview requires H.264 or HEVC video");
  const auto free_packet = [](AVPacket* value) { av_packet_free(&value); };
  std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
  if (!packet)
    return absl::ResourceExhaustedError("Cannot allocate camera metadata packet");
  for (unsigned index = 0; index < context->nb_streams; ++index)
    if (context->streams[index] != video)
      context->streams[index]->discard = AVDISCARD_ALL;
  const double unit = av_q2d(video->time_base);
  double origin = video->start_time == AV_NOPTS_VALUE ? 0 : video->start_time * unit;
  if (video->start_time == AV_NOPTS_VALUE) {
    bool found = false;
    for (size_t count = 0; count < 100000 && !interrupt.Poll(); ++count) {
      if (av_read_frame(context, packet.get()) < 0)
        break;
      if (packet->stream_index == video->index && packet->pts != AV_NOPTS_VALUE) {
        origin = packet->pts * unit;
        found = true;
      }
      av_packet_unref(packet.get());
      if (found)
        break;
    }
    if (!interrupt.Status().ok())
      return interrupt.Status();
    if (!found)
      return absl::DataLossError("Camera video has no bounded presentation origin");
  }
  double seconds = video->duration > 0 ? video->duration * unit : 0;
  // Matroska DURATION is a track end timestamp, not a length. In particular,
  // container duration includes audio tails and cannot define this timeline.
  if (seconds <= 0)
    if (const auto* tag = av_dict_get(video->metadata, "DURATION", nullptr, 0)) {
      int64_t end = 0;
      if (av_parse_time(&end, tag->value, 1) == 0)
        seconds = end / double(AV_TIME_BASE) - origin;
    }
  const double fps = av_q2d(video->avg_frame_rate);
  if (seconds <= 0 && video->nb_frames > 0 && std::isfinite(fps) && fps > 0)
    seconds = video->nb_frames / fps;
  if (seconds <= 0 && context->duration > 0) {
    // Container duration is only a seek hint. Confirm the actual video end
    // from bounded compressed packets, including containers without track tags.
    const auto hint = av_rescale_q(context->duration, AVRational{1, AV_TIME_BASE}, video->time_base);
    if (av_seek_frame(context, video->index, hint, AVSEEK_FLAG_BACKWARD) >= 0) {
      double end = origin;
      for (size_t count = 0; count < 100000 && !interrupt.Poll(); ++count) {
        const int result = av_read_frame(context, packet.get());
        if (result == AVERROR_EOF) {
          seconds = end - origin;
          break;
        }
        if (result < 0)
          break;
        if (packet->stream_index == video->index && packet->pts != AV_NOPTS_VALUE) {
          const double length = packet->duration > 0 ? packet->duration * unit
              : std::isfinite(fps) && fps > 0        ? 1.0 / fps
                                                     : 0;
          if (length > 0)
            end = std::max(end, packet->pts * unit + length);
        }
        av_packet_unref(packet.get());
      }
    }
  }
  interrupt.Poll();
  if (!interrupt.Status().ok())
    return interrupt.Status();
  if (!std::isfinite(origin) || origin < 0 || origin * 1e9 >= std::numeric_limits<int64_t>::max())
    return absl::DataLossError("Camera video has an invalid presentation origin");
  if (Clock::now() >= deadline || !std::isfinite(seconds) || seconds <= 0 ||
      (seconds + origin) * 1e9 >= std::numeric_limits<int64_t>::max())
    return absl::DataLossError("Camera video duration is missing, invalid or exceeded its inspection deadline");
  return SourceCameraTrack{
      codec->codec_id == AV_CODEC_ID_H264 ? media::VideoCodec::kH264 : media::VideoCodec::kHevc,
      codec->width,
      codec->height,
      static_cast<int64_t>(std::llround(origin * 1e9)),
      static_cast<int64_t>(std::llround(seconds * 1e9)),
      g_strcmp0(context->iformat->name, "avi") == 0 ? "avidemux" : "matroskademux"};
}
} // namespace hm::ui
