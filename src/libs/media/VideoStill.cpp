#include "hstream/src/libs/media/VideoStill.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <vector>

namespace hm::media {
namespace {
using Clock = std::chrono::steady_clock;
struct Reader {
  AVFormatContext* format{avformat_alloc_context()};
  AVCodecContext* decoder{};
  AVPacket* packet{av_packet_alloc()};
  AVFrame* frame{av_frame_alloc()};
  SwsContext* scaler{};
  Clock::time_point deadline;
  const std::function<bool()>& cancelled;
  Reader(int64_t timeout, const std::function<bool()>& cancel) : cancelled(cancel) {
    const auto now = Clock::now();
    const auto duration = std::chrono::nanoseconds(timeout);
    deadline = duration > Clock::time_point::max() - now ? Clock::time_point::max() : now + duration;
    if (format)
      format->interrupt_callback = {
          [](void* value) { return static_cast<Reader*>(value)->Check().ok() ? 0 : 1; }, this};
  }
  ~Reader() {
    sws_freeContext(scaler);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
  }
  absl::Status Check() const {
    if (cancelled && cancelled())
      return absl::CancelledError("Camera preview cancelled");
    if (Clock::now() >= deadline)
      return absl::DeadlineExceededError("Camera preview timed out");
    return absl::OkStatus();
  }
  absl::Status Error(const char* operation, int code) const {
    const auto status = Check();
    if (!status.ok())
      return status;
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    return absl::DataLossError(std::string(operation) + ": " + message);
  }
};
} // namespace
absl::StatusOr<VideoStill> ReadVideoStill(
    const std::string& source,
    bool live,
    int64_t time_ns,
    int maximum_dimension,
    int64_t maximum_source_pixels,
    int64_t timeout_ns,
    const std::function<bool()>& cancelled) {
  if (time_ns < 0 || timeout_ns <= 0 || maximum_dimension < 2 || maximum_dimension > 8192 ||
      maximum_source_pixels < 4 || maximum_source_pixels > INT_MAX / 4)
    return absl::InvalidArgumentError("Invalid camera preview bounds");
  std::error_code error;
  if (live ? (source.rfind("rtsp://", 0) != 0 && source.rfind("rtsps://", 0) != 0)
           : !std::filesystem::is_regular_file(source, error))
    return absl::InvalidArgumentError("Camera preview requires a recording or RTSP feed");
  Reader reader(timeout_ns, cancelled);
  if (!reader.format || !reader.packet || !reader.frame)
    return absl::ResourceExhaustedError("Cannot allocate camera preview reader");
  auto status = reader.Check();
  if (!status.ok())
    return status;
  AVDictionary* options = nullptr;
  if (live)
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
  int result = avformat_open_input(&reader.format, source.c_str(), nullptr, &options);
  av_dict_free(&options);
  if (result < 0)
    return reader.Error("Cannot open camera preview", result);
  // Stream inspection can itself open a decoder. Apply the same allocation
  // and thread bounds there as to the explicit still decoder below.
  std::vector<AVDictionary*> probe_options(reader.format->nb_streams, nullptr);
  for (auto& probe : probe_options) {
    av_dict_set_int(&probe, "max_pixels", maximum_source_pixels, 0);
    av_dict_set(&probe, "threads", "1", 0);
  }
  result = avformat_find_stream_info(reader.format, probe_options.data());
  for (auto& probe : probe_options)
    av_dict_free(&probe);
  if (result < 0)
    return reader.Error("Cannot inspect camera preview", result);
  const int index = av_find_best_stream(reader.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (index < 0)
    return reader.Error("Camera preview has no video", index);
  auto* stream = reader.format->streams[index];
  const auto* parameters = stream->codecpar;
  if ((parameters->codec_id != AV_CODEC_ID_H264 && parameters->codec_id != AV_CODEC_ID_HEVC) || parameters->width < 2 ||
      parameters->height < 2 || int64_t(parameters->width) * parameters->height > maximum_source_pixels)
    return absl::InvalidArgumentError("Camera preview requires bounded H.264 or HEVC video");
  const auto* codec = avcodec_find_decoder(parameters->codec_id);
  reader.decoder = avcodec_alloc_context3(codec);
  if (!codec || !reader.decoder)
    return absl::UnavailableError("Camera preview decoder unavailable");
  result = avcodec_parameters_to_context(reader.decoder, parameters);
  if (result < 0)
    return reader.Error("Cannot configure camera preview", result);
  reader.decoder->thread_count = 1;
  reader.decoder->max_pixels = maximum_source_pixels;
  if ((result = avcodec_open2(reader.decoder, codec, nullptr)) < 0)
    return reader.Error("Cannot open camera preview decoder", result);
  const int64_t origin = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
  const auto seek = av_rescale_q(time_ns, AVRational{1, 1000000000}, stream->time_base);
  if (seek > INT64_MAX - std::max<int64_t>(0, origin))
    return absl::OutOfRangeError("Camera preview seek overflow");
  const auto target = origin + seek;
  if (!live && time_ns && (result = av_seek_frame(reader.format, index, target, AVSEEK_FLAG_BACKWARD)) < 0)
    return reader.Error("Cannot seek camera preview", result);
  bool draining = false;
  for (;;) {
    status = reader.Check();
    if (!status.ok())
      return status;
    result = avcodec_receive_frame(reader.decoder, reader.frame);
    if (result == AVERROR(EAGAIN)) {
      if (draining)
        return absl::DataLossError("Camera preview decoder did not drain");
      do {
        av_packet_unref(reader.packet);
        result = av_read_frame(reader.format, reader.packet);
        status = reader.Check();
        if (!status.ok())
          return status;
      } while (result >= 0 && reader.packet->stream_index != index);
      if (result < 0 && result != AVERROR_EOF)
        return reader.Error("Cannot read camera preview", result);
      draining = result == AVERROR_EOF;
      result = avcodec_send_packet(reader.decoder, draining ? nullptr : reader.packet);
      if (result < 0)
        return reader.Error("Cannot decode camera preview", result);
      continue;
    }
    if (result < 0)
      return reader.Error("No camera frame at the selected preview time", result);
    auto* frame = reader.frame;
    const auto pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE)
      return absl::DataLossError("Camera preview has no presentation timestamp");
    if (!live && pts < target) {
      av_frame_unref(frame);
      continue;
    }
    if (frame->width != parameters->width || frame->height != parameters->height)
      return absl::DataLossError("Camera dimensions changed during preview");
    if (pts < origin || (origin < 0 && pts > INT64_MAX + origin))
      return absl::OutOfRangeError("Camera preview timestamp overflow");
    const int longest = std::max(frame->width, frame->height);
    const int width = std::max(1, int(int64_t(frame->width) * std::min(longest, maximum_dimension) / longest));
    const int height = std::max(1, int(int64_t(frame->height) * std::min(longest, maximum_dimension) / longest));
    reader.scaler = sws_getContext(
        frame->width,
        frame->height,
        AVPixelFormat(frame->format),
        width,
        height,
        AV_PIX_FMT_RGBA,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);
    if (!reader.scaler)
      return absl::UnavailableError("Cannot scale camera preview");
    VideoStill output{
        frame->width,
        frame->height,
        width,
        height,
        av_rescale_q(pts - origin, stream->time_base, AVRational{1, 1000000000}),
        {}};
    output.rgba.resize(size_t(width) * height * 4);
    uint8_t* planes[]{output.rgba.data(), nullptr, nullptr, nullptr};
    int strides[]{width * 4, 0, 0, 0};
    if (sws_scale(reader.scaler, frame->data, frame->linesize, 0, frame->height, planes, strides) != height)
      return absl::DataLossError("Incomplete camera preview conversion");
    status = reader.Check();
    if (!status.ok())
      return status;
    return output;
  }
}
} // namespace hm::media
