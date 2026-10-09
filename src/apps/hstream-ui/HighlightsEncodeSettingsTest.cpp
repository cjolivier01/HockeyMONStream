#include "src/apps/hstream-ui/HighlightsEncodeSettings.h"

#include <cmath>
#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

// Reads the value that follows an option, so that argument ordering stays an
// implementation detail here.
QString option_value(const QStringList& arguments, const QString& option) {
  const qsizetype index = arguments.indexOf(option);
  return index < 0 || index + 1 >= arguments.size() ? QString() : arguments.at(index + 1);
}

// Shaped like a real `ffprobe -show_streams -show_format -of json` run over the
// published 4K program archive.
QByteArray archive_probe(
    const QString& codec,
    const QString& pixel_format,
    const QString& video_bit_rate,
    bool with_audio) {
  QString audio;
  if (with_audio) {
    audio =
        R"(,{"index":1,"codec_name":"aac","codec_type":"audio","sample_rate":"48000","channels":2,)"
        R"("bit_rate":"128000"})";
  }
  return QString(
             R"({"streams":[{"index":0,"codec_name":"%1","codec_type":"video","width":3840,"height":2160,)"
             R"("pix_fmt":"%2","color_range":"tv","color_space":"bt709","color_transfer":"bt709",)"
             R"("color_primaries":"bt709","r_frame_rate":"60000/1001","avg_frame_rate":"60000/1001",)"
             R"("bit_rate":%3}%4],)"
             R"("format":{"duration":"172.757000","size":"637000000","bit_rate":"29528000"}})")
      .arg(codec, pixel_format, video_bit_rate, audio)
      .toUtf8();
}

} // namespace

int main() {
  using hm::ui::ArchiveMediaInfo;
  using hm::ui::DeriveHighlightsEncodeSettings;
  using hm::ui::HighlightsEncodeSettings;
  using hm::ui::ParseArchiveMediaInfo;
  using hm::ui::ParseFfmpegEncoderList;

  bool ok = true;
  QString error;

  ArchiveMediaInfo info;
  ok &= expect(
      ParseArchiveMediaInfo(archive_probe("hevc", "yuv420p", R"("29400000")", true), &info, &error),
      "a complete ffprobe report must parse");
  ok &= expect(
      info.width == 3840 && info.height == 2160 && info.video_codec == "hevc" && info.pixel_format == "yuv420p" &&
          info.bit_depth == 8,
      "geometry, codec, and pixel format must come from the first video stream");
  ok &= expect(
      std::abs(info.frame_rate - 60000.0 / 1001.0) < 1e-9, "rational frame rates must not be rounded to an integer");
  ok &= expect(info.video_bit_rate == 29400000, "a per-stream bitrate must be taken verbatim");
  ok &= expect(
      info.has_audio && info.audio_codec == "aac" && info.audio_sample_rate == 48000 && info.audio_channels == 2 &&
          info.audio_bit_rate == 128000,
      "the first audio stream must be described");
  ok &= expect(
      info.color_space == "bt709" && info.color_range == "tv" && std::abs(info.duration_seconds - 172.757) < 1e-6,
      "colour tags and duration must survive");

  ArchiveMediaInfo fallback;
  ok &= expect(
      ParseArchiveMediaInfo(archive_probe("hevc", "yuv420p", R"("N/A")", true), &fallback, &error) &&
          fallback.video_bit_rate == 29528000 - 128000,
      "a container without a per-stream video bitrate must fall back to the container rate less the audio");
  ArchiveMediaInfo size_based;
  ok &= expect(
      ParseArchiveMediaInfo(
          R"({"streams":[{"codec_type":"video","width":1920,"height":1080,"r_frame_rate":"30/1"}],)"
          R"("format":{"duration":"100","size":"125000000"}})",
          &size_based,
          &error) &&
          size_based.video_bit_rate == 10000000,
      "with no reported rate at all the file size over the duration must be used");
  ArchiveMediaInfo ten_bit;
  ok &= expect(
      ParseArchiveMediaInfo(archive_probe("hevc", "yuv420p10le", R"("29400000")", false), &ten_bit, &error) &&
          ten_bit.bit_depth == 10 && !ten_bit.has_audio,
      "pixel-format suffixes must reveal the sample depth, and a silent archive must report no audio");
  ArchiveMediaInfo semiplanar;
  ok &= expect(
      ParseArchiveMediaInfo(archive_probe("hevc", "nv12", R"("29400000")", false), &semiplanar, &error) &&
          semiplanar.bit_depth == 8,
      "nv12 names its chroma layout, not a twelve bit sample depth");
  ArchiveMediaInfo rejected;
  ok &= expect(
      !ParseArchiveMediaInfo(R"({"streams":[{"codec_type":"audio"}],"format":{}})", &rejected, &error) &&
          !error.isEmpty(),
      "an archive with no video stream must be refused");
  ok &= expect(!ParseArchiveMediaInfo("{not json", &rejected, &error), "unparseable ffprobe output must be refused");

  const QStringList encoders = ParseFfmpegEncoderList(
      "Encoders:\n"
      " V..... = Video\n"
      " A..... = Audio\n"
      " S..... = Subtitle\n"
      " .F.... = Frame-level multithreading\n"
      " ------\n"
      " V....D libx264              libx264 H.264 / AVC (codec h264)\n"
      " V....D h264_nvenc           NVIDIA NVENC H.264 encoder (codec h264)\n"
      " V....D hevc_nvenc           NVIDIA NVENC hevc encoder (codec hevc)\n"
      " A....D aac                  AAC (Advanced Audio Coding)\n");
  ok &= expect(
      encoders.size() == 4 && encoders.contains("hevc_nvenc") && encoders.contains("libx264") &&
          encoders.contains("aac") && !encoders.contains("Video"),
      "the encoder list must hold codec names and skip the flag legend");

  const QProcessEnvironment no_overrides;
  const QStringList with_nvenc{"libx264", "libx265", "h264_nvenc", "hevc_nvenc", "aac"};
  const QStringList software_only{"libx264", "libx265", "aac"};

  const HighlightsEncodeSettings hardware = DeriveHighlightsEncodeSettings(info, with_nvenc, no_overrides);
  ok &= expect(
      hardware.video_encoder == "hevc_nvenc" && hardware.hardware && hardware.hevc && hardware.video_preset == "p7",
      "an HEVC source with NVENC present must re-encode in hardware at the slowest preset");
  ok &= expect(
      std::abs(hardware.bits_per_pixel - 0.0591) < 0.0002 && hardware.video_bit_rate == 29400000,
      "the target bitrate must reproduce the source's bits per pixel at the same geometry");
  ok &= expect(
      option_value(hardware.arguments, "-c:v") == "hevc_nvenc" &&
          option_value(hardware.arguments, "-b:v") == "29400000" &&
          option_value(hardware.arguments, "-maxrate") == "58800000" &&
          option_value(hardware.arguments, "-bufsize") == "117600000" &&
          option_value(hardware.arguments, "-g") == "120",
      "rate control must be derived from the target bitrate and the source frame rate");
  ok &= expect(
      option_value(hardware.arguments, "-profile:v") == "main" &&
          option_value(hardware.arguments, "-pix_fmt") == "yuv420p" &&
          option_value(hardware.arguments, "-tag:v") == "hvc1",
      "an 8-bit HEVC output must stay in main profile and carry the hvc1 tag");
  ok &= expect(
      option_value(hardware.arguments, "-colorspace") == "bt709" &&
          option_value(hardware.arguments, "-color_range") == "tv",
      "known colour tags must be carried onto the output");
  ok &= expect(
      option_value(hardware.arguments, "-c:a") == "aac" && option_value(hardware.arguments, "-b:a") == "128000" &&
          option_value(hardware.arguments, "-ar") == "48000" && option_value(hardware.arguments, "-ac") == "2",
      "audio must be re-encoded at the source's rate and layout");
  ok &= expect(
      hardware.arguments.contains("-multipass") && hardware.arguments.contains("-temporal-aq"),
      "the NVENC quality options must be present on the hardware path");

  const HighlightsEncodeSettings software = DeriveHighlightsEncodeSettings(info, software_only, no_overrides);
  ok &= expect(
      software.video_encoder == "libx265" && !software.hardware && software.hevc &&
          software.video_preset == "medium" && !software.arguments.contains("-multipass"),
      "without NVENC the HEVC source must fall back to libx265 and drop the hardware-only options");

  ArchiveMediaInfo h264 = info;
  h264.video_codec = "h264";
  const HighlightsEncodeSettings avc = DeriveHighlightsEncodeSettings(h264, with_nvenc, no_overrides);
  ok &= expect(
      avc.video_encoder == "h264_nvenc" && !avc.hevc && option_value(avc.arguments, "-profile:v") == "high" &&
          !avc.arguments.contains("-tag:v"),
      "an H.264 source must stay H.264 and must not be tagged hvc1");

  ArchiveMediaInfo deep = info;
  deep.bit_depth = 10;
  const HighlightsEncodeSettings deep_hardware = DeriveHighlightsEncodeSettings(deep, with_nvenc, no_overrides);
  ok &= expect(
      option_value(deep_hardware.arguments, "-profile:v") == "main10" &&
          option_value(deep_hardware.arguments, "-pix_fmt") == "p010le",
      "a 10-bit HEVC source must stay 10-bit through NVENC");
  const HighlightsEncodeSettings deep_software = DeriveHighlightsEncodeSettings(deep, software_only, no_overrides);
  ok &= expect(
      option_value(deep_software.arguments, "-pix_fmt") == "yuv420p10le",
      "libx265 takes planar 10-bit rather than NVENC's semi-planar format");
  ArchiveMediaInfo deep_avc = deep;
  deep_avc.video_codec = "h264";
  ok &= expect(
      option_value(DeriveHighlightsEncodeSettings(deep_avc, with_nvenc, no_overrides).arguments, "-pix_fmt") ==
          "yuv420p",
      "the H.264 path must not claim a 10-bit output NVENC cannot produce here");

  ArchiveMediaInfo silent = info;
  silent.has_audio = false;
  ok &= expect(
      !DeriveHighlightsEncodeSettings(silent, with_nvenc, no_overrides).arguments.contains("-c:a"),
      "a silent archive must not get audio options");

  ArchiveMediaInfo rateless = info;
  rateless.video_bit_rate = 0;
  const HighlightsEncodeSettings guessed = DeriveHighlightsEncodeSettings(rateless, with_nvenc, no_overrides);
  ok &= expect(
      std::abs(guessed.bits_per_pixel - 0.06) < 1e-9 && guessed.video_bit_rate > 29000000,
      "an archive with no usable bitrate must fall back to a measured bits-per-pixel figure");

  QProcessEnvironment overrides;
  overrides.insert("HSTREAM_UI_HIGHLIGHTS_VIDEO_ENCODER", "libx264");
  overrides.insert("HSTREAM_UI_HIGHLIGHTS_VIDEO_PRESET", "veryslow");
  overrides.insert("HSTREAM_UI_HIGHLIGHTS_BITS_PER_PIXEL", "0.1");
  const HighlightsEncodeSettings overridden = DeriveHighlightsEncodeSettings(info, with_nvenc, overrides);
  ok &= expect(
      overridden.video_encoder == "libx264" && !overridden.hardware && !overridden.hevc &&
          overridden.video_preset == "veryslow",
      "the environment must be able to pin the encoder and preset");
  ok &= expect(
      std::abs(overridden.bits_per_pixel - 0.1) < 1e-12 && overridden.video_bit_rate == 49716683,
      "an overridden bits-per-pixel figure must drive the target bitrate");

  const HighlightsEncodeSettings unknown_support = DeriveHighlightsEncodeSettings(info, {}, no_overrides);
  ok &= expect(
      unknown_support.video_encoder == "hevc_nvenc",
      "with no encoder list available the first preference must be used unconditionally");
  return ok ? 0 : 1;
}
