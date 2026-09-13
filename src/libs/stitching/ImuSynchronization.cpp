#include "hstream/src/libs/stitching/ImuSynchronization.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
}

#include "hstream/src/libs/stitching/CalibrationFrameExif.h"

namespace hm::stitching {
absl::StatusOr<SynchronizationMethod> parse_synchronization_method(const std::string& name) {
  if (name == "audio")
    return SynchronizationMethod::kAudio;
  if (name == "imu")
    return SynchronizationMethod::kImu;
  if (name == "auto")
    return SynchronizationMethod::kAuto;
  return absl::InvalidArgumentError("stitching.sync_method must be audio, imu, or auto; got: " + name);
}

namespace imu {
namespace {
constexpr double kDuration = 30.0;
constexpr double kRate = 200.0;
constexpr double kMaxOffset = 5.0;
constexpr size_t kMaxSamples = 200000;

uint64_t read_integer(const unsigned char* p, size_t n, bool little = false) {
  uint64_t value = 0;
  for (size_t i = 0; i < n; ++i)
    value = (value << 8) | p[little ? n - i - 1 : i];
  return value;
}
double read_double(const unsigned char* p) {
  const uint64_t bits = read_integer(p, 8, true);
  double value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

absl::Status parse_gpmf(
    const unsigned char* data,
    size_t size,
    double seconds,
    double duration,
    std::vector<Sample>& samples,
    unsigned depth = 0) {
  if (depth > 8)
    return absl::DataLossError("GPMF nesting is too deep");
  const unsigned char* scale_header = nullptr;
  for (size_t pos = 0; pos < size;) {
    if (size - pos < 8)
      return absl::DataLossError("Truncated GPMF header");
    const auto* header = data + pos;
    const size_t width = header[5], count = read_integer(header + 6, 2);
    const size_t bytes = width * count, padded = (bytes + 3) & ~size_t(3);
    if (padded > size - pos - 8)
      return absl::DataLossError("Truncated GPMF payload");
    const auto* payload = header + 8;
    const std::string key(reinterpret_cast<const char*>(header), 4);
    if (header[4] == 0) {
      const auto status = parse_gpmf(payload, bytes, seconds, duration, samples, depth + 1);
      if (!status.ok())
        return status;
    } else if (key == "SCAL") {
      scale_header = header;
    } else if (key == "GYRO" && samples.empty()) {
      if (header[4] != 's' || width != 6 || count == 0)
        return absl::UnimplementedError("Unsupported GPMF gyroscope format (expected signed 16-bit triples)");
      std::array<double, 3> scale{1, 1, 1};
      if (scale_header) {
        const size_t scale_width = scale_header[5], scale_count = read_integer(scale_header + 6, 2);
        if ((scale_header[4] != 'l' && scale_header[4] != 's') || (scale_count != 1 && scale_count != 3) ||
            scale_width != (scale_header[4] == 'l' ? 4u : 2u))
          return absl::UnimplementedError("Unsupported GPMF gyro scale");
        for (size_t axis = 0; axis < 3; ++axis) {
          const uint64_t raw = read_integer(scale_header + 8 + (axis % scale_count) * scale_width, scale_width);
          scale[axis] = scale_width == 4 ? static_cast<int32_t>(raw) : static_cast<int16_t>(raw);
          if (scale[axis] == 0)
            return absl::DataLossError("Zero GPMF gyro scale");
        }
      }
      for (size_t i = 0; i < count; ++i) {
        Sample sample{seconds + duration * i / count, 0, 0, 0};
        double values[3];
        for (size_t axis = 0; axis < 3; ++axis)
          values[axis] = static_cast<int16_t>(read_integer(payload + i * width + axis * 2, 2)) / scale[axis];
        sample.x = values[0];
        sample.y = values[1];
        sample.z = values[2];
        samples.push_back(sample);
      }
    }
    pos += 8 + padded;
  }
  return absl::OkStatus();
}

absl::StatusOr<std::vector<Sample>> read_insta(const std::string& video, const frame_exif::Insta360Clock& clock) {
  // 200k samples cover over 30s even at 3.2kHz; never load a full match's IMU data.
  const bool raw = clock.scale == 0.001;
  const size_t stride = raw ? 20 : 56;
  const auto record = frame_exif::ReadInsta360Record(video, 3, stride * kMaxSamples);
  if (!record.ok())
    return record.status();
  if (!record->has_value())
    return absl::NotFoundError("No Insta360 gyroscope record");
  const auto& bytes = record->value().data;
  if (record->value().full_size % stride != 0)
    return absl::DataLossError("Invalid Insta360 gyro record size");
  std::vector<Sample> samples;
  for (size_t pos = 0; pos + stride <= bytes.size(); pos += stride) {
    const auto* p = bytes.data() + pos;
    const double time = (read_integer(p, 8, true) / 1000.0 - clock.origin) * clock.scale - clock.gyro_offset;
    if (time > kDuration + kMaxOffset)
      break;
    double axes[3];
    for (size_t axis = 0; axis < 3; ++axis) {
      // Uniform unit scaling cancels in normalized magnitude correlation.
      axes[axis] = raw ? static_cast<double>(read_integer(p + 14 + axis * 2, 2, true)) - 32768.0
                       : read_double(p + 32 + axis * 8);
    }
    samples.push_back({time, axes[0], axes[1], axes[2]});
  }
  return samples;
}

absl::Status validate(const std::vector<Sample>& samples) {
  if (samples.size() < 100 || samples.size() > kMaxSamples)
    return absl::FailedPreconditionError("IMU synchronization requires 100 to 200000 gyro samples per camera");
  double previous = -std::numeric_limits<double>::infinity();
  for (const auto& sample : samples) {
    if (!std::isfinite(sample.seconds) || !std::isfinite(std::hypot(sample.x, sample.y, sample.z)) ||
        sample.seconds <= previous)
      return absl::DataLossError("Non-finite or non-increasing gyroscope samples");
    if (std::isfinite(previous) && sample.seconds - previous > 0.1)
      return absl::FailedPreconditionError("Gyroscope timestamps contain gaps larger than 100ms");
    previous = sample.seconds;
  }
  return absl::OkStatus();
}

std::vector<double> resample(const std::vector<Sample>& samples, double start, size_t count) {
  std::vector<double> result;
  size_t index = 0;
  for (size_t i = 0; i < count; ++i) {
    const double time = start + i / kRate;
    while (index + 1 < samples.size() - 1 && samples[index + 1].seconds < time)
      ++index;
    const auto& a = samples[index];
    const auto& b = samples[index + 1];
    const double fraction = (time - a.seconds) / (b.seconds - a.seconds);
    const double av = std::hypot(a.x, a.y, a.z), bv = std::hypot(b.x, b.y, b.z);
    result.push_back(av + fraction * (bv - av));
  }
  return result;
}
} // namespace

absl::StatusOr<std::vector<Sample>> ParseGpmf(const unsigned char* data, size_t size, double seconds, double duration) {
  if (!data || size > 4 * 1024 * 1024 || !std::isfinite(seconds) || !std::isfinite(duration) || duration <= 0)
    return absl::InvalidArgumentError("Invalid GPMF packet or timing");
  std::vector<Sample> result;
  const auto status = parse_gpmf(data, size, seconds, duration, result);
  if (!status.ok())
    return status;
  return result;
}

absl::StatusOr<Recording> Read(const std::string& video) {
  // avformat demuxes metadata packets only; no codec is opened and no image is decoded.
  AVFormatContext* context = avformat_alloc_context();
  if (!context)
    return absl::ResourceExhaustedError("Cannot allocate metadata demuxer");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  context->interrupt_callback = {
      [](void* value) {
        return int(std::chrono::steady_clock::now() > *static_cast<const decltype(deadline)*>(value));
      },
      const_cast<void*>(static_cast<const void*>(&deadline))};
  if (avformat_open_input(&context, video.c_str(), nullptr, nullptr) < 0)
    return absl::NotFoundError("Cannot open IMU video: " + video);
  const auto close = [](AVFormatContext* p) { avformat_close_input(&p); };
  std::unique_ptr<AVFormatContext, decltype(close)> owner(context, close);
  Recording result;
  double origin = 0;
  int gyro_stream = -1;
  for (unsigned i = 0; i < context->nb_streams; ++i) {
    const AVStream* stream = context->streams[i];
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && result.fps == 0) {
      result.fps = av_q2d(stream->avg_frame_rate);
      origin = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time * av_q2d(stream->time_base);
    }
    if (stream->codecpar->codec_tag == MKTAG('g', 'p', 'm', 'd'))
      gyro_stream = i;
  }
  if (!std::isfinite(result.fps) || result.fps <= 0)
    return absl::FailedPreconditionError("IMU synchronization requires a video frame rate");
  const auto clock = frame_exif::ReadInsta360Clock(video);
  if (!clock.ok())
    return clock.status();
  if (clock->has_value()) {
    auto samples = read_insta(video, clock->value());
    if (!samples.ok())
      return samples.status();
    result.gyro = std::move(*samples);
  } else {
    if (gyro_stream < 0)
      return absl::NotFoundError("No supported native gyro telemetry (GoPro GPMF or Insta360 trailer): " + video);
    const auto free_packet = [](AVPacket* p) { av_packet_free(&p); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    if (!packet)
      return absl::ResourceExhaustedError("Cannot allocate metadata packet");
    const double time_base = av_q2d(context->streams[gyro_stream]->time_base);
    int code;
    while ((code = av_read_frame(context, packet.get())) >= 0) {
      if (packet->stream_index == gyro_stream) {
        if (packet->pts == AV_NOPTS_VALUE)
          return absl::DataLossError("GPMF packet has no timestamp");
        const double time = packet->pts * time_base - origin;
        if (time > kDuration + kMaxOffset)
          break;
        auto samples = ParseGpmf(packet->data, packet->size, time, packet->duration * time_base);
        if (!samples.ok())
          return samples.status();
        if (result.gyro.size() + samples->size() > kMaxSamples)
          return absl::ResourceExhaustedError("Too many GPMF samples in synchronization window");
        result.gyro.insert(result.gyro.end(), samples->begin(), samples->end());
      }
      av_packet_unref(packet.get());
    }
    if (code < 0 && code != AVERROR_EOF)
      return absl::DataLossError("Failed or timed out reading GPMF packets");
  }
  const auto status = validate(result.gyro);
  if (!status.ok())
    return status;
  return result;
}

absl::StatusOr<double> EstimateOffset(const std::vector<Sample>& left, const std::vector<Sample>& right) {
  for (const auto* samples : {&left, &right}) {
    const auto status = validate(*samples);
    if (!status.ok())
      return status;
  }
  const double left_start = std::max(0.0, left.front().seconds), right_start = std::max(0.0, right.front().seconds);
  const double duration = std::min({kDuration, left.back().seconds - left_start, right.back().seconds - right_start});
  if (duration < 0.5)
    return absl::FailedPreconditionError("Insufficient gyro duration for synchronization");
  const size_t n = static_cast<size_t>(duration * kRate);
  const auto a = resample(left, left_start, n), b = resample(right, right_start, n);
  const int max_lag = std::min(
      static_cast<int>(std::min<double>(n / 2, kMaxOffset * kRate + std::abs(left_start - right_start) * kRate)),
      static_cast<int>(n / 2));
  std::vector<std::pair<int, double>> scores;
  double best = -1;
  int best_lag = 0;
  for (int lag = -max_lag; lag <= max_lag; ++lag) {
    const double offset = left_start - right_start - lag / kRate;
    if (std::abs(offset) > kMaxOffset)
      continue;
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    const int begin = std::max(0, -lag), end = std::min(static_cast<int>(n), static_cast<int>(n) - lag);
    const double count = end - begin;
    for (int i = begin; i < end; ++i) {
      const double x = a[i], y = b[i + lag];
      sa += x;
      sb += y;
      saa += x * x;
      sbb += y * y;
      sab += x * y;
    }
    const double va = saa - sa * sa / count, vb = sbb - sb * sb / count;
    if (va <= 1e-12 * std::max(1.0, saa) || vb <= 1e-12 * std::max(1.0, sbb))
      continue;
    const double score = (sab - sa * sb / count) / std::sqrt(va * vb);
    scores.emplace_back(lag, score);
    if (score > best) {
      best = score;
      best_lag = lag;
    }
  }
  if (best < 0.7)
    return absl::FailedPreconditionError(
        "IMU correlation is weak or constant; use audio or record shared rig movement");
  for (const auto& [lag, score] : scores) {
    if (std::abs(lag - best_lag) > 20 && score > best - 0.05)
      return absl::FailedPreconditionError("IMU correlation has ambiguous peaks; use audio synchronization");
  }
  const double offset = left_start - right_start - best_lag / kRate;
  std::cout << "IMU synchronization: left-minus-right skip " << offset << " seconds, correlation " << best << '\n';
  return offset;
}
} // namespace imu

absl::StatusOr<std::pair<double, double>> synchronize_by_imu(const std::string& left, const std::string& right) {
  const auto a = imu::Read(left);
  if (!a.ok())
    return a.status();
  const auto b = imu::Read(right);
  if (!b.ok())
    return b.status();
  const auto offset = imu::EstimateOffset(a->gyro, b->gyro);
  if (!offset.ok())
    return offset.status();
  return std::make_pair(std::max(0.0, *offset) * a->fps, std::max(0.0, -*offset) * b->fps);
}
} // namespace hm::stitching
