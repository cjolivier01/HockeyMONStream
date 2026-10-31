#include "hstream/src/libs/stitching/Synchronization.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <numeric>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include "hstream/src/libs/stitching/CrossCorrelation.h"

namespace hm::stitching {
namespace {
struct AudioInput {
  AVFormatContext* format{nullptr};
  AVCodecContext* codec{nullptr};
  SwrContext* resampler{nullptr};
  AVPacket* packet{nullptr};
  AVFrame* frame{nullptr};
  std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  ~AudioInput() {
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&resampler);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
  }
  absl::Status Open(const std::string& file) {
    if ((avformat_version() >> 16) != LIBAVFORMAT_VERSION_MAJOR ||
        (avcodec_version() >> 16) != LIBAVCODEC_VERSION_MAJOR || (avutil_version() >> 16) != LIBAVUTIL_VERSION_MAJOR)
      return absl::FailedPreconditionError("Synchronization requires matching FFmpeg headers and runtime libraries");
    format = avformat_alloc_context();
    if (!format)
      return absl::ResourceExhaustedError("Cannot allocate synchronization demuxer");
    format->interrupt_callback = {
        [](void* value) { return int(std::chrono::steady_clock::now() > static_cast<AudioInput*>(value)->deadline); },
        this};
    if (avformat_open_input(&format, file.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(format, nullptr) < 0)
      return absl::NotFoundError("Cannot read synchronization source: " + file);
    return absl::OkStatus();
  }
};

absl::StatusOr<std::vector<float>> ReadAudio(const CameraRecordingWindow& window, double seconds, int& rate) {
  std::vector<float> output;
  for (const auto& chapter : window.chapters) {
    if (seconds <= 0)
      break;
    const double duration = std::min(seconds, chapter.duration_seconds);
    AudioInput input;
    auto status = input.Open(chapter.file);
    if (!status.ok())
      return status;
    const int audio = av_find_best_stream(input.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    const int video = av_find_best_stream(input.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (audio < 0 || video < 0)
      return absl::FailedPreconditionError("Synchronization requires video and audio tracks: " + chapter.file);
    auto* stream = input.format->streams[audio];
    const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder || !(input.codec = avcodec_alloc_context3(decoder)) ||
        avcodec_parameters_to_context(input.codec, stream->codecpar) < 0 ||
        avcodec_open2(input.codec, decoder, nullptr) < 0)
      return absl::FailedPreconditionError("Cannot decode synchronization audio: " + chapter.file);
    if (rate == 0)
      rate = input.codec->sample_rate;
    if (rate <= 0 || input.codec->sample_rate != rate)
      return absl::FailedPreconditionError("Synchronization audio tracks must have matching sample rates");
#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    if (swr_alloc_set_opts2(
            &input.resampler,
            &mono,
            AV_SAMPLE_FMT_FLT,
            rate,
            &input.codec->ch_layout,
            input.codec->sample_fmt,
            rate,
            0,
            nullptr) < 0)
      return absl::ResourceExhaustedError("Cannot allocate synchronization audio conversion");
#else
    const auto layout = input.codec->channel_layout ? input.codec->channel_layout
                                                    : av_get_default_channel_layout(input.codec->channels);
    input.resampler = swr_alloc_set_opts(
        nullptr, AV_CH_LAYOUT_MONO, AV_SAMPLE_FMT_FLT, rate, layout, input.codec->sample_fmt, rate, 0, nullptr);
#endif
    if (!input.resampler || swr_init(input.resampler) < 0)
      return absl::FailedPreconditionError("Cannot initialize synchronization audio conversion");
    const auto* video_stream = input.format->streams[video];
    const double origin =
        video_stream->start_time == AV_NOPTS_VALUE ? 0 : video_stream->start_time * av_q2d(video_stream->time_base);
    const double start = chapter.start_seconds + origin;
    for (unsigned index = 0; index < input.format->nb_streams; ++index)
      if (int(index) != audio)
        input.format->streams[index]->discard = AVDISCARD_ALL;
    if (av_seek_frame(input.format, audio, std::llround(start / av_q2d(stream->time_base)), AVSEEK_FLAG_BACKWARD) < 0)
      return absl::FailedPreconditionError("Cannot seek synchronization audio to camera start: " + chapter.file);
    input.packet = av_packet_alloc();
    input.frame = av_frame_alloc();
    if (!input.packet || !input.frame)
      return absl::ResourceExhaustedError("Cannot allocate synchronization audio frame");
    const size_t base = output.size(), count = std::llround(duration * rate);
    output.resize(base + count, 0);
    bool finished = false;
    int code = 0;
    while (!finished && (code = av_read_frame(input.format, input.packet)) >= 0) {
      if (input.packet->stream_index == audio) {
        if (avcodec_send_packet(input.codec, input.packet) < 0)
          return absl::DataLossError("Cannot decode synchronization audio packet");
        for (;;) {
          const int decoded = avcodec_receive_frame(input.codec, input.frame);
          if (decoded == AVERROR(EAGAIN) || decoded == AVERROR_EOF)
            break;
          if (decoded < 0 || input.frame->best_effort_timestamp == AV_NOPTS_VALUE)
            return absl::DataLossError("Synchronization audio is missing decoded timestamps");
          const int64_t position =
              std::llround((input.frame->best_effort_timestamp * av_q2d(stream->time_base) - start) * rate);
          std::vector<float> pcm(input.frame->nb_samples);
          uint8_t* destination = reinterpret_cast<uint8_t*>(pcm.data());
          const int converted = swr_convert(
              input.resampler,
              &destination,
              pcm.size(),
              const_cast<const uint8_t**>(input.frame->extended_data),
              input.frame->nb_samples);
          if (converted < 0)
            return absl::DataLossError("Cannot convert synchronization audio");
          for (int i = std::max<int64_t>(0, -position); i < converted && position + i < int64_t(count); ++i)
            output[base + position + i] = pcm[i];
          finished = position + converted >= int64_t(count);
          av_frame_unref(input.frame);
          if (finished)
            break;
        }
      }
      av_packet_unref(input.packet);
    }
    if (code < 0 && code != AVERROR_EOF)
      return absl::DataLossError("Failed or timed out reading synchronization audio");
    seconds -= duration;
  }
  return output;
}
} // namespace

absl::StatusOr<CameraRecordingWindow> CameraRecordingWindowAt(
    const std::vector<std::string>& files,
    double start_seconds,
    double duration_seconds) {
  if (files.empty() || !std::isfinite(start_seconds) || start_seconds < 0 || !std::isfinite(duration_seconds) ||
      duration_seconds <= 0)
    return absl::InvalidArgumentError("Camera synchronization requires chapters and finite positive window bounds");
  CameraRecordingWindow result{{}, 0, 0};
  double skipped = 0;
  for (const auto& file : files) {
    AudioInput input;
    const auto status = input.Open(file);
    if (!status.ok())
      return status;
    const int video = av_find_best_stream(input.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video < 0)
      return absl::FailedPreconditionError("Camera chapter has no video: " + file);
    const auto* stream = input.format->streams[video];
    const double fps = av_q2d(stream->avg_frame_rate);
    const double duration = stream->duration == AV_NOPTS_VALUE ? 0 : stream->duration * av_q2d(stream->time_base);
    if (!std::isfinite(fps) || fps <= 0 || !std::isfinite(duration) || duration <= 0)
      return absl::FailedPreconditionError("Camera chapter requires a known duration and frame rate: " + file);
    if (result.fps != 0 && std::abs(result.fps - fps) > 1e-5)
      return absl::FailedPreconditionError("Camera chapters have different frame rates");
    result.fps = fps;
    if (start_seconds >= skipped + duration) {
      skipped += duration;
      continue;
    }
    const double local = std::max(0.0, start_seconds - skipped);
    const double take = std::min(duration - local, duration_seconds - result.duration_seconds);
    result.chapters.push_back({file, local, take});
    result.duration_seconds += take;
    if (result.duration_seconds >= duration_seconds)
      break;
    skipped += duration;
  }
  if (result.chapters.empty())
    return absl::OutOfRangeError(
        "Camera start " + std::to_string(start_seconds) + " seconds reaches or exceeds " + files.front() +
        " recording duration " + std::to_string(skipped) + " seconds");
  return result;
}

absl::StatusOr<std::pair<double, double>> synchronize_by_audio(
    const CameraRecordingWindow& left,
    const CameraRecordingWindow& right) {
  const double seconds = std::min({15.0, left.duration_seconds, right.duration_seconds});
  if (seconds < 0.5)
    return absl::FailedPreconditionError("Camera starts leave less than half a second for audio synchronization");
  int rate = 0;
  auto a = ReadAudio(left, seconds, rate);
  if (!a.ok())
    return a.status();
  auto b = ReadAudio(right, seconds, rate);
  if (!b.ok())
    return b.status();
  if (a->size() != b->size() || a->empty())
    return absl::FailedPreconditionError("Camera starts leave unequal audio synchronization windows");
  for (auto* samples : {&*a, &*b}) {
    const double mean = std::accumulate(samples->begin(), samples->end(), 0.0) / samples->size();
    for (auto& value : *samples)
      value -= mean;
  }
  const auto correlation = full_correlate_fft(*a, *b);
  const auto peak = std::max_element(correlation.begin(), correlation.end());
  const auto lag = int64_t(a->size()) - 1 - std::distance(correlation.begin(), peak);
  double aa = 0, bb = 0, ab = 0;
  const int64_t begin = std::max<int64_t>(0, -lag), end = std::min<int64_t>(b->size(), a->size() - lag);
  for (int64_t i = begin; i < end; ++i) {
    const double x = (*a)[i + lag], y = (*b)[i];
    aa += x * x;
    bb += y * y;
    ab += x * y;
  }
  if (end - begin < int64_t(a->size() / 2) || aa <= 0 || bb <= 0 || ab / std::sqrt(aa * bb) < 0.1)
    return absl::FailedPreconditionError(
        "Audio synchronization could not find a reliable match. Set each camera's source start to roughly the same "
        "event before retrying synchronization.");
  const double offset = double(lag) / rate;
  std::cout << "Audio synchronization residual left-minus-right skip: " << offset << " seconds\n";
  return std::make_pair(std::max(0.0, offset), std::max(0.0, -offset));
}
} // namespace hm::stitching
