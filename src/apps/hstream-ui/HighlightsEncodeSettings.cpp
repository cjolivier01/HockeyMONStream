#include "src/apps/hstream-ui/HighlightsEncodeSettings.h"

#include <algorithm>
#include <cmath>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QRegularExpression>

namespace hm::ui {
namespace {

// Used only when the source reports no usable bitrate at all. Close to what the
// stitched and program archives actually measure.
constexpr double kFallbackBitsPerPixel = 0.06;
constexpr qint64 kMinimumVideoBitRate = 1000000;
constexpr qint64 kMaximumVideoBitRate = 400000000;
constexpr qint64 kMinimumAudioBitRate = 128000;
constexpr qint64 kMaximumAudioBitRate = 320000;
constexpr qint64 kDefaultAudioBitRate = 192000;

bool fail(QString* error, const QString& message) {
  if (error)
    *error = message;
  return false;
}

// ffprobe reports most numbers as strings, and "N/A" for anything it could not
// determine.
double number_field(const QJsonObject& object, const QString& key) {
  const QJsonValue value = object.value(key);
  if (value.isDouble())
    return value.toDouble();
  if (!value.isString())
    return 0.0;
  bool ok = false;
  const double parsed = value.toString().toDouble(&ok);
  return ok && std::isfinite(parsed) ? parsed : 0.0;
}

QString string_field(const QJsonObject& object, const QString& key) {
  const QString value = object.value(key).toString();
  return value == "unknown" || value == "N/A" ? QString() : value;
}

double parse_rational(const QString& text) {
  const qsizetype slash = text.indexOf('/');
  if (slash < 0) {
    bool ok = false;
    const double value = text.toDouble(&ok);
    return ok && std::isfinite(value) ? value : 0.0;
  }
  bool numerator_ok = false;
  bool denominator_ok = false;
  const double numerator = text.left(slash).toDouble(&numerator_ok);
  const double denominator = text.mid(slash + 1).toDouble(&denominator_ok);
  if (!numerator_ok || !denominator_ok || denominator == 0.0)
    return 0.0;
  const double value = numerator / denominator;
  return std::isfinite(value) && value > 0.0 ? value : 0.0;
}

int pixel_format_bit_depth(const QString& pixel_format, const QJsonObject& stream) {
  const double raw = number_field(stream, "bits_per_raw_sample");
  if (raw >= 9.0 && raw <= 16.0)
    return static_cast<int>(raw);
  // nv12 and friends spell their chroma layout in digits rather than their
  // depth, and they are all eight bit.
  static const QRegularExpression semiplanar_layout(R"(^nv(?:12|21|16|24|42)$)");
  if (semiplanar_layout.match(pixel_format).hasMatch())
    return 8;
  static const QRegularExpression depth_suffix(R"((\d{2})(?:le|be)?$)");
  const QRegularExpressionMatch match = depth_suffix.match(pixel_format);
  if (match.hasMatch()) {
    const int depth = match.captured(1).toInt();
    if (depth >= 9 && depth <= 16)
      return depth;
  }
  return 8;
}

QString first_available(const QStringList& preferences, const QStringList& available) {
  if (available.isEmpty())
    return preferences.value(0);
  for (const QString& candidate : preferences) {
    if (available.contains(candidate))
      return candidate;
  }
  return preferences.value(0);
}

} // namespace

bool ParseArchiveMediaInfo(const QByteArray& ffprobe_json, ArchiveMediaInfo* info, QString* error) {
  if (!info)
    return fail(error, "No media info destination was supplied");
  QJsonParseError parse_error;
  const QJsonDocument document = QJsonDocument::fromJson(ffprobe_json, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    return fail(error, "Could not read ffprobe output: " + parse_error.errorString());
  const QJsonObject root = document.object();
  const QJsonObject format = root.value("format").toObject();

  ArchiveMediaInfo parsed;
  parsed.duration_seconds = number_field(format, "duration");

  bool saw_video = false;
  qint64 reported_video_bit_rate = 0;
  for (const QJsonValue& entry : root.value("streams").toArray()) {
    const QJsonObject stream = entry.toObject();
    const QString type = stream.value("codec_type").toString();
    if (type == "video" && !saw_video) {
      saw_video = true;
      parsed.width = static_cast<int>(number_field(stream, "width"));
      parsed.height = static_cast<int>(number_field(stream, "height"));
      parsed.video_codec = string_field(stream, "codec_name");
      parsed.pixel_format = string_field(stream, "pix_fmt");
      parsed.bit_depth = pixel_format_bit_depth(parsed.pixel_format, stream);
      parsed.color_space = string_field(stream, "color_space");
      parsed.color_primaries = string_field(stream, "color_primaries");
      parsed.color_transfer = string_field(stream, "color_transfer");
      parsed.color_range = string_field(stream, "color_range");
      parsed.frame_rate = parse_rational(stream.value("r_frame_rate").toString());
      if (parsed.frame_rate <= 0.0)
        parsed.frame_rate = parse_rational(stream.value("avg_frame_rate").toString());
      reported_video_bit_rate = static_cast<qint64>(number_field(stream, "bit_rate"));
      if (parsed.duration_seconds <= 0.0)
        parsed.duration_seconds = number_field(stream, "duration");
    } else if (type == "audio" && !parsed.has_audio) {
      parsed.has_audio = true;
      parsed.audio_codec = string_field(stream, "codec_name");
      parsed.audio_sample_rate = static_cast<int>(number_field(stream, "sample_rate"));
      parsed.audio_channels = static_cast<int>(number_field(stream, "channels"));
      parsed.audio_bit_rate = static_cast<qint64>(number_field(stream, "bit_rate"));
    }
  }
  if (!saw_video)
    return fail(error, "The archive has no video stream");

  // Containers often carry a per-stream bitrate; MKV usually does not, so fall
  // back to the container rate less the audio, and then to file size over
  // duration.
  parsed.video_bit_rate = reported_video_bit_rate;
  if (parsed.video_bit_rate <= 0) {
    const qint64 container = static_cast<qint64>(number_field(format, "bit_rate"));
    if (container > 0)
      parsed.video_bit_rate = std::max<qint64>(0, container - std::max<qint64>(0, parsed.audio_bit_rate));
  }
  if (parsed.video_bit_rate <= 0 && parsed.duration_seconds > 0.0) {
    const double size_bytes = number_field(format, "size");
    if (size_bytes > 0.0)
      parsed.video_bit_rate = static_cast<qint64>(size_bytes * 8.0 / parsed.duration_seconds);
  }

  *info = parsed;
  return true;
}

QStringList ParseFfmpegEncoderList(const QString& output) {
  static const QRegularExpression row(R"(^\s*[VAS][A-Z0-9.]{5}\s+([A-Za-z0-9_.-]+))");
  QStringList encoders;
  const QStringList lines = output.split('\n');
  for (const QString& line : lines) {
    const QRegularExpressionMatch match = row.match(line);
    if (match.hasMatch())
      encoders << match.captured(1);
  }
  return encoders;
}

HighlightsEncodeSettings DeriveHighlightsEncodeSettings(
    const ArchiveMediaInfo& info,
    const QStringList& available_encoders,
    const QProcessEnvironment& env) {
  HighlightsEncodeSettings settings;

  const bool source_is_h264 = info.video_codec == "h264" || info.video_codec == "avc1";
  QStringList preferences;
  if (source_is_h264)
    preferences << "h264_nvenc" << "libx264" << "hevc_nvenc" << "libx265";
  else
    preferences << "hevc_nvenc" << "libx265" << "h264_nvenc" << "libx264";
  settings.video_encoder = env.value("HSTREAM_UI_HIGHLIGHTS_VIDEO_ENCODER").trimmed();
  if (settings.video_encoder.isEmpty())
    settings.video_encoder = first_available(preferences, available_encoders);
  settings.hardware = settings.video_encoder.endsWith("_nvenc");
  settings.hevc = settings.video_encoder.contains("hevc") || settings.video_encoder.contains("x265");

  settings.video_preset = env.value("HSTREAM_UI_HIGHLIGHTS_VIDEO_PRESET").trimmed();
  if (settings.video_preset.isEmpty())
    settings.video_preset = settings.hardware ? "p7" : "medium";

  // Hold the source's bits per pixel per frame, so a future scaled output lands
  // at a comparable quality instead of a comparable bitrate.
  const double source_pixels_per_second =
      static_cast<double>(info.width) * static_cast<double>(info.height) * info.frame_rate;
  settings.bits_per_pixel = kFallbackBitsPerPixel;
  if (info.video_bit_rate > 0 && source_pixels_per_second > 0.0)
    settings.bits_per_pixel = static_cast<double>(info.video_bit_rate) / source_pixels_per_second;
  bool override_ok = false;
  const double bits_per_pixel_override =
      env.value("HSTREAM_UI_HIGHLIGHTS_BITS_PER_PIXEL").trimmed().toDouble(&override_ok);
  if (override_ok && std::isfinite(bits_per_pixel_override) && bits_per_pixel_override > 0.0)
    settings.bits_per_pixel = bits_per_pixel_override;

  const double target = settings.bits_per_pixel * source_pixels_per_second;
  settings.video_bit_rate = std::clamp<qint64>(
      std::isfinite(target) && target > 0.0 ? static_cast<qint64>(std::llround(target)) : kMinimumVideoBitRate,
      kMinimumVideoBitRate,
      kMaximumVideoBitRate);

  // NVENC's H.264 encoder is 8-bit only here, so only the HEVC path follows the
  // source into 10 bits.
  const bool ten_bit = info.bit_depth >= 10 && settings.hevc;
  if (settings.hevc) {
    settings.profile = ten_bit ? "main10" : "main";
    settings.pixel_format = ten_bit ? (settings.hardware ? "p010le" : "yuv420p10le") : "yuv420p";
  } else {
    settings.profile = "high";
    settings.pixel_format = "yuv420p";
  }

  const qint64 gop = std::max<qint64>(1, std::llround(std::max(1.0, info.frame_rate) * 2.0));
  QStringList args{"-c:v", settings.video_encoder, "-preset", settings.video_preset};
  if (settings.hardware) {
    args << "-tune" << "hq" << "-rc" << "vbr" << "-multipass" << "fullres" << "-rc-lookahead" << "32"
         << "-spatial-aq" << "1" << "-temporal-aq" << "1" << "-b_ref_mode" << "middle";
  }
  args << "-b:v" << QString::number(settings.video_bit_rate) << "-maxrate"
       << QString::number(std::min(kMaximumVideoBitRate, settings.video_bit_rate * 2)) << "-bufsize"
       << QString::number(std::min(kMaximumVideoBitRate * 2, settings.video_bit_rate * 4)) << "-g"
       << QString::number(gop) << "-profile:v" << settings.profile << "-pix_fmt" << settings.pixel_format;
  if (!info.color_space.isEmpty())
    args << "-colorspace" << info.color_space;
  if (!info.color_primaries.isEmpty())
    args << "-color_primaries" << info.color_primaries;
  if (!info.color_transfer.isEmpty())
    args << "-color_trc" << info.color_transfer;
  if (!info.color_range.isEmpty())
    args << "-color_range" << info.color_range;

  if (info.has_audio) {
    settings.audio_bit_rate = info.audio_bit_rate > 0
                                  ? std::clamp(info.audio_bit_rate, kMinimumAudioBitRate, kMaximumAudioBitRate)
                                  : kDefaultAudioBitRate;
    args << "-c:a" << "aac" << "-b:a" << QString::number(settings.audio_bit_rate);
    if (info.audio_sample_rate > 0)
      args << "-ar" << QString::number(info.audio_sample_rate);
    if (info.audio_channels > 0)
      args << "-ac" << QString::number(info.audio_channels);
  }
  if (settings.hevc)
    args << "-tag:v" << "hvc1";
  settings.arguments = args;
  return settings;
}

} // namespace hm::ui
