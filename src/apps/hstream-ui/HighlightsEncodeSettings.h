#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace hm::ui {

// What `ffprobe -show_streams -show_format -of json` says about an archive.
struct ArchiveMediaInfo {
  int width{0};
  int height{0};
  double frame_rate{0.0};
  QString video_codec;
  QString pixel_format;
  int bit_depth{8};
  QString color_space;
  QString color_primaries;
  QString color_transfer;
  QString color_range;
  // Bits per second of the video stream, after the stream/format/size
  // fallbacks. Zero when nothing usable was reported.
  qint64 video_bit_rate{0};
  bool has_audio{false};
  QString audio_codec;
  int audio_sample_rate{0};
  int audio_channels{0};
  qint64 audio_bit_rate{0};
  double duration_seconds{0.0};
};

bool ParseArchiveMediaInfo(const QByteArray& ffprobe_json, ArchiveMediaInfo* info, QString* error = nullptr);

// Encoder choice and rate control for a highlights reel cut from an archive.
struct HighlightsEncodeSettings {
  QString video_encoder;
  QString video_preset;
  QString profile;
  QString pixel_format;
  // Bits per pixel per frame measured from the source, and the bitrate that
  // reproduces it at the output geometry.
  double bits_per_pixel{0.0};
  qint64 video_bit_rate{0};
  qint64 audio_bit_rate{0};
  bool hardware{false};
  bool hevc{false};
  // Complete output options: -c:v through -c:a, including colour tags and
  // -tag:v hvc1 where it applies. Does not include the output filename.
  QStringList arguments;
};

// available_encoders is the set named by `ffmpeg -encoders`; pass an empty list
// when it is unknown and the first preference is used unconditionally.
HighlightsEncodeSettings DeriveHighlightsEncodeSettings(
    const ArchiveMediaInfo& info,
    const QStringList& available_encoders,
    const QProcessEnvironment& env);

// Encoder names parsed out of `ffmpeg -hide_banner -encoders` output.
QStringList ParseFfmpegEncoderList(const QString& output);

} // namespace hm::ui
