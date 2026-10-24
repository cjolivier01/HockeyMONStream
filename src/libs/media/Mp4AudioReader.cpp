#include "hstream/src/libs/media/Mp4AudioReader.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>

namespace hm::media {
namespace {
using Clock = std::chrono::steady_clock;
int Channels(const AVCodecParameters& parameters) {
#if LIBAVCODEC_VERSION_MAJOR >= 59
  return parameters.ch_layout.nb_channels;
#else
  return parameters.channels;
#endif
}
int Channels(const AVFrame& frame) {
#if LIBAVUTIL_VERSION_MAJOR >= 57
  return frame.ch_layout.nb_channels;
#else
  return frame.channels;
#endif
}
absl::Status Error(const char* operation, int code) {
  char text[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(code, text, sizeof(text));
  return absl::DataLossError(std::string(operation) + ": " + text);
}
} // namespace
struct Mp4AudioReader::Impl {
  Mp4AudioReaderConfig config;
  std::atomic<bool> cancelled{false};
  Clock::time_point deadline;
  AVFormatContext* file{};
  AVCodecContext* decoder{};
  AVFrame* frame{};
  AVPacket* packet{};
  AVStream* audio{};
  AudioFormat format{};
  int64_t target{}, previous_end{INT64_MIN};
  bool retired{false}, positioned{false}, draining{false}, ended{false};
  ~Impl() {
    Close();
  }
  void Close() {
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&file);
    audio = nullptr;
  }
  void Start() {
    const auto now = Clock::now();
    const auto delay = std::chrono::nanoseconds(config.operation_timeout_ns);
    deadline = delay > Clock::time_point::max() - now ? Clock::time_point::max() : now + delay;
  }
  absl::Status Check() const {
    if (cancelled.load(std::memory_order_relaxed))
      return absl::CancelledError("MP4 audio cancelled");
    if (retired)
      return absl::FailedPreconditionError("MP4 audio retired after failure");
    if (Clock::now() >= deadline)
      return absl::DeadlineExceededError("MP4 audio operation timed out");
    return absl::OkStatus();
  }
  absl::Status Retire(absl::Status status) {
    retired = true;
    return status;
  }
  absl::Status Failure(const char* operation, int code) {
    const auto status = Check();
    return Retire(status.ok() ? Error(operation, code) : status);
  }
};
Mp4AudioReader::Mp4AudioReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Mp4AudioReader::~Mp4AudioReader() = default;
absl::StatusOr<std::unique_ptr<Mp4AudioReader>> Mp4AudioReader::Create(const Mp4AudioReaderConfig& config) {
  if (config.operation_timeout_ns <= 0 || !config.maximum_packet_bytes || config.maximum_packet_bytes > INT_MAX ||
      !config.maximum_frame_samples || config.maximum_frame_samples > INT_MAX / (2 * sizeof(float)))
    return absl::InvalidArgumentError("MP4 audio requires positive finite packet, frame and time bounds");
  if ((avformat_version() >> 16) != LIBAVFORMAT_VERSION_MAJOR ||
      (avcodec_version() >> 16) != LIBAVCODEC_VERSION_MAJOR || (avutil_version() >> 16) != LIBAVUTIL_VERSION_MAJOR)
    return absl::FailedPreconditionError("MP4 audio requires matching FFmpeg headers and libraries");
  auto impl = std::make_unique<Impl>();
  impl->config = config;
  return std::unique_ptr<Mp4AudioReader>(new Mp4AudioReader(std::move(impl)));
}
absl::Status Mp4AudioReader::Open(const std::filesystem::path& path) {
  auto& impl = *impl_;
  impl.Start();
  const auto status = impl.Check();
  if (!status.ok())
    return status;
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error)
    return absl::NotFoundError("MP4 audio requires a local regular file: " + path.string());
  impl.Close();
  impl.positioned = impl.draining = impl.ended = false;
  impl.file = avformat_alloc_context();
  if (!impl.file)
    return impl.Retire(absl::ResourceExhaustedError("Cannot allocate MP4 audio demuxer"));
  impl.file->interrupt_callback = {[](void* p) { return static_cast<Impl*>(p)->Check().ok() ? 0 : 1; }, &impl};
  AVDictionary* options{};
  av_dict_set(&options, "protocol_whitelist", "file", 0);
  av_dict_set(&options, "enable_drefs", "0", 0);
  const auto* demuxer = av_find_input_format("mov");
  if (!demuxer) {
    av_dict_free(&options);
    return impl.Retire(absl::UnavailableError("FFmpeg MP4 demuxer is unavailable"));
  }
  const int opened = avformat_open_input(&impl.file, path.c_str(), demuxer, &options);
  av_dict_free(&options);
  if (opened < 0)
    return impl.Failure("Cannot open MP4 audio", opened);
  for (unsigned i = 0; i < impl.file->nb_streams; ++i) {
    auto* stream = impl.file->streams[i];
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
      stream->discard = AVDISCARD_ALL;
      continue;
    }
    if (impl.audio)
      return impl.Retire(absl::InvalidArgumentError("MP4 audio requires exactly one audio track"));
    impl.audio = stream;
  }
  if (!impl.audio)
    return impl.Retire(absl::InvalidArgumentError("MP4 has no audio track"));
  const auto* parameters = impl.audio->codecpar;
  if (parameters->codec_id != AV_CODEC_ID_AAC ||
      (parameters->profile != FF_PROFILE_UNKNOWN && parameters->profile != FF_PROFILE_AAC_LOW))
    return impl.Retire(absl::UnimplementedError("MP4 audio requires AAC-LC"));
  if (parameters->sample_rate <= 0 || parameters->sample_rate > 96000 || Channels(*parameters) < 1 ||
      Channels(*parameters) > 2 || impl.audio->time_base.num <= 0 || impl.audio->time_base.den <= 0)
    return impl.Retire(absl::InvalidArgumentError("MP4 audio requires mono/stereo AAC with a finite sample clock"));
  const auto* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
  if (!codec)
    return impl.Retire(absl::UnavailableError("Native AAC decoder is unavailable"));
  impl.decoder = avcodec_alloc_context3(codec);
  impl.packet = av_packet_alloc();
  impl.frame = av_frame_alloc();
  if (!impl.decoder || !impl.packet || !impl.frame)
    return impl.Retire(absl::ResourceExhaustedError("Cannot allocate AAC decoder"));
  int result = avcodec_parameters_to_context(impl.decoder, parameters);
  if (result < 0)
    return impl.Failure("Cannot configure AAC decoder", result);
  impl.decoder->pkt_timebase = impl.audio->time_base;
  impl.decoder->thread_count = 1;
  result = avcodec_open2(impl.decoder, codec, nullptr);
  if (result < 0)
    return impl.Failure("Cannot open AAC decoder", result);
  impl.format = {parameters->sample_rate, Channels(*parameters)};
  const auto checked = impl.Check();
  return checked.ok() ? checked : impl.Retire(checked);
}
absl::Status Mp4AudioReader::Seek(int64_t sample) {
  auto& impl = *impl_;
  impl.Start();
  const auto status = impl.Check();
  if (!status.ok())
    return status;
  if (!impl.audio)
    return absl::FailedPreconditionError("Open MP4 audio before seeking");
  if (sample < 0)
    return absl::InvalidArgumentError("MP4 audio seek must be nonnegative");
  // AAC-LC uses a 1024-sample overlap. Decode the preceding access unit to
  // restore that overlap after a seek, then discard it before delivery.
  const int64_t warmup = sample - 1024;
  const int64_t ticks = av_rescale_q_rnd(warmup, {1, impl.format.rate}, impl.audio->time_base, AV_ROUND_DOWN);
  if (ticks == INT64_MIN || ticks == INT64_MAX)
    return absl::OutOfRangeError("MP4 audio seek overflows the stream clock");
  const int result = av_seek_frame(impl.file, impl.audio->index, ticks, AVSEEK_FLAG_BACKWARD);
  if (result < 0)
    return impl.Failure("Cannot seek MP4 audio", result);
  avcodec_flush_buffers(impl.decoder);
  av_packet_unref(impl.packet);
  av_frame_unref(impl.frame);
  impl.target = sample;
  impl.previous_end = INT64_MIN;
  impl.draining = impl.ended = false;
  impl.positioned = true;
  return absl::OkStatus();
}
absl::StatusOr<std::optional<AudioFrame>> Mp4AudioReader::Read() {
  auto& impl = *impl_;
  impl.Start();
  for (;;) {
    const auto status = impl.Check();
    if (!status.ok())
      return impl.Retire(status);
    if (!impl.positioned)
      return absl::FailedPreconditionError("Seek MP4 audio before reading");
    if (impl.ended)
      return std::nullopt;
    int result = avcodec_receive_frame(impl.decoder, impl.frame);
    if (result == AVERROR_EOF) {
      impl.ended = true;
      return std::nullopt;
    }
    if (result == AVERROR(EAGAIN)) {
      if (impl.draining)
        return impl.Retire(absl::DataLossError("AAC decoder stalled while draining"));
      for (;;) {
        const auto checked = impl.Check();
        if (!checked.ok())
          return impl.Retire(checked);
        result = av_read_frame(impl.file, impl.packet);
        if (result < 0)
          break;
        if (impl.packet->stream_index == impl.audio->index)
          break;
        av_packet_unref(impl.packet);
      }
      if (result == AVERROR_EOF) {
        impl.draining = true;
        result = avcodec_send_packet(impl.decoder, nullptr);
      } else if (result < 0) {
        return impl.Failure("Cannot read MP4 audio packet", result);
      } else {
        if (impl.packet->size <= 0 || size_t(impl.packet->size) > impl.config.maximum_packet_bytes)
          return impl.Retire(absl::ResourceExhaustedError("AAC packet exceeds its configured bound"));
        result = avcodec_send_packet(impl.decoder, impl.packet);
        av_packet_unref(impl.packet);
      }
      if (result < 0)
        return impl.Failure("Cannot submit AAC packet", result);
      continue;
    }
    if (result < 0)
      return impl.Failure("Cannot decode AAC packet", result);
    const auto& frame = *impl.frame;
    if (frame.sample_rate != impl.format.rate || Channels(frame) != impl.format.channels || frame.nb_samples <= 0 ||
        size_t(frame.nb_samples) > impl.config.maximum_frame_samples ||
        (frame.format != AV_SAMPLE_FMT_FLTP && frame.format != AV_SAMPLE_FMT_FLT) ||
        impl.decoder->profile != FF_PROFILE_AAC_LOW || frame.pts == AV_NOPTS_VALUE)
      return impl.Retire(absl::DataLossError("AAC frame changed format or exceeded its sample bound"));
    const int64_t sample = av_rescale_q(frame.pts, impl.audio->time_base, {1, impl.format.rate});
    if (sample == INT64_MIN || sample > INT64_MAX - frame.nb_samples || sample < impl.previous_end)
      return impl.Retire(absl::DataLossError("AAC frame clock overflows or overlaps preceding audio"));
    impl.previous_end = sample + frame.nb_samples;
    if (impl.previous_end <= impl.target) {
      av_frame_unref(impl.frame);
      continue;
    }
    AudioFrame output{sample, impl.format, std::vector<float>(size_t(frame.nb_samples) * impl.format.channels)};
    for (int i = 0; i < frame.nb_samples; ++i)
      for (int channel = 0; channel < impl.format.channels; ++channel) {
        const auto* plane =
            reinterpret_cast<const float*>(frame.extended_data[frame.format == AV_SAMPLE_FMT_FLTP ? channel : 0]);
        const float value = plane[frame.format == AV_SAMPLE_FMT_FLTP ? i : i * impl.format.channels + channel];
        if (!std::isfinite(value))
          return impl.Retire(absl::DataLossError("AAC decoded nonfinite PCM"));
        output.pcm[size_t(i) * impl.format.channels + channel] = value;
      }
    av_frame_unref(impl.frame);
    return std::optional<AudioFrame>(std::move(output));
  }
}
void Mp4AudioReader::Cancel() noexcept {
  impl_->cancelled.store(true, std::memory_order_relaxed);
}
AudioFormat Mp4AudioReader::format() const {
  return impl_->format;
}
} // namespace hm::media
