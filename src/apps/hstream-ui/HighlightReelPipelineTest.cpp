#include "src/apps/hstream-ui/HighlightReelPipeline.h"
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtGui/QImage>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>
#include <cmath>
#include <iostream>
using namespace hm::ui;
namespace {
QApplication* app;
bool run(const QString& cmd, const QStringList& args, QByteArray* result = nullptr) {
  QProcess p;
  p.start(cmd, args);
  if (!p.waitForFinished(30000) || p.exitCode() != 0) {
    std::cerr << p.readAllStandardError().constData();
    return false;
  }
  if (result)
    *result = p.readAllStandardOutput();
  return true;
}
bool wait(HighlightReelPipeline& p, int limit = 20000, bool expect_cancel = false) {
  for (int elapsed = 0; elapsed < limit; elapsed += 10) {
    app->processEvents();
    const auto s = p.Poll();
    if (s.finished) {
      std::cerr << "finished: " << s.error.toStdString() << " cancelled=" << s.cancelled << '\n';
      return s.error.isEmpty() && s.cancelled == expect_cancel;
    }
    QThread::msleep(10);
  }
  std::cerr << "Reel timed out\n";
  p.Cancel();
  return false;
}
bool pixel(const QString& file, double time, int x, int y, int* r, int* g, int* b) {
  QByteArray bytes;
  if (!run(
          "ffmpeg",
          {"-v",
           "error",
           "-ss",
           QString::number(time),
           "-i",
           file,
           "-frames:v",
           "1",
           "-vf",
           QString("crop=2:2:%1:%2").arg(x).arg(y),
           "-pix_fmt",
           "rgb24",
           "-f",
           "rawvideo",
           "pipe:1"},
          &bytes) ||
      bytes.size() < 3)
    return false;
  *r = uchar(bytes[0]);
  *g = uchar(bytes[1]);
  *b = uchar(bytes[2]);
  return true;
}
qint64 audio_level(const QString& file, double time, double length) {
  QByteArray pcm;
  // Decode preroll before trimming; an input seek to zero can omit AAC's
  // negative-timestamp packet and reset its overlap state in the oracle.
  if (!run(
          "ffmpeg",
          {"-v",
           "error",
           "-i",
           file,
           "-ss",
           QString::number(time),
           "-t",
           QString::number(length),
           "-vn",
           "-ac",
           "1",
           "-ar",
           "48000",
           "-f",
           "s16le",
           "pipe:1"},
          &pcm) ||
      pcm.size() < 2)
    return -1;
  qint64 energy = 0;
  for (int i = 0; i + 1 < pcm.size(); i += 2)
    energy += std::abs(qint16(quint8(pcm[i]) | (quint16(quint8(pcm[i + 1])) << 8)));
  return energy / (pcm.size() / 2);
}
bool increasing_timestamps(const QString& file) {
  QByteArray packets;
  if (!run(
          "ffprobe",
          {"-v",
           "error",
           "-select_streams",
           "v:0",
           "-show_entries",
           "packet=pts_time,dts_time",
           "-of",
           "csv=p=0",
           file},
          &packets))
    return false;
  double previous_pts = -1, previous_dts = -1;
  for (const QByteArray& row : packets.split('\n')) {
    if (row.trimmed().isEmpty())
      continue;
    const auto fields = row.split(',');
    bool valid_pts = false, valid_dts = false;
    const double pts = fields.value(0).toDouble(&valid_pts), dts = fields.value(1).toDouble(&valid_dts);
    if (!valid_pts || !valid_dts || pts <= previous_pts || dts <= previous_dts) {
      std::cerr << "Duplicate or reversed encoded frame timestamps: " << row.constData() << '\n';
      return false;
    }
    previous_pts = pts;
    previous_dts = dts;
  }
  return previous_pts >= 0;
}
bool transient_span(const QString& file, double first_ms, double last_ms) {
  QByteArray pcm;
  if (!run("ffmpeg", {"-v", "error", "-i", file, "-vn", "-ac", "1", "-ar", "48000", "-f", "s16le", "pipe:1"}, &pcm))
    return false;
  qint64 first = -1, last = -1;
  for (int i = 0; i + 1 < pcm.size(); i += 2)
    if (std::abs(qint16(quint8(pcm[i]) | (quint16(quint8(pcm[i + 1])) << 8))) > 3000) {
      if (first < 0)
        first = i / 2;
      last = i / 2;
    }
  if (first < 0 || std::abs(first / 48.0 - first_ms) > 2 || std::abs(last / 48.0 - last_ms) > 2) {
    std::cerr << "Misaligned or missing audio transient: " << first / 48.0 << " .. " << last / 48.0 << " ms\n";
    return false;
  }
  return true;
}
} // namespace
int main(int argc, char** argv) {
  QApplication application(argc, argv);
  app = &application;
  QTemporaryDir dir;
  const QString archive = dir.filePath("source.mp4"), output = dir.filePath("reel.mp4");
  // Independent source and decoding oracle; production pipeline never launches FFmpeg.
  if (!run(
          "ffmpeg",
          {"-v",
           "error",
           "-f",
           "lavfi",
           "-i",
           "color=c=blue:s=640x360:r=30:d=5",
           "-f",
           "lavfi",
           "-i",
           "sine=frequency=440:sample_rate=48000:duration=5",
           "-c:v",
           "libx264",
           "-g",
           "30",
           "-c:a",
           "aac",
           archive}))
    return 1;
  HighlightInterval clip;
  clip.event_mode = false;
  clip.start_ms = 10550;
  clip.end_ms = 12600;
  HighlightAnnotation box;
  box.kind = "box";
  box.start_ms = 10550;
  box.end_ms = 11050;
  box.x = .1;
  box.y = .1;
  box.dx = .4;
  box.dy = .4;
  box.thickness = .02;
  box.blink = false;
  clip.annotations = {box};
  HighlightInterval card;
  card.is_card = true;
  card.card.heading = "WHISTLE!";
  card.card.background = "#ff0000";
  card.card.duration_ms = 1000;
  auto second = clip;
  second.start_ms = 13350;
  second.end_ms = 14750;
  second.annotations.clear();
  HighlightReelPipeline::Request r;
  r.archive_path = archive;
  r.archive_offset_ms = 10000;
  r.asset_root = dir.path();
  r.output_path = output;
  r.items = {clip, card, second};
  r.media.width = 640;
  r.media.height = 360;
  r.media.frame_rate = 30;
  r.media.video_codec = "h264";
  r.media.has_audio = true;
  HighlightReelPipeline pipeline;
  QString error;
  if (!pipeline.Start(r, &error) || !wait(pipeline)) {
    std::cerr << error.toStdString();
    return 1;
  }
  QByteArray duration;
  if (!run(
          "ffprobe",
          {"-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1", output},
          &duration) ||
      std::abs(duration.trimmed().toDouble() - 4.45) > .08) {
    std::cerr << "Bad mixed duration " << duration.constData();
    return 1;
  }
  if (!increasing_timestamps(output))
    return 1;
  int red, green, blue;
  if (!pixel(output, .2, 62, 36, &red, &green, &blue) || red < 150 || green < 150) {
    std::cerr << "Missing authored box\n";
    return 1;
  }
  if (!pixel(output, 1, 62, 36, &red, &green, &blue) || blue < 150 || red > 80) {
    std::cerr << "Expired box remained\n";
    return 1;
  }
  if (!pixel(output, 2.3, 10, 10, &red, &green, &blue) || red < 150 || blue > 80) {
    std::cerr << "Missing card boundary\n";
    return 1;
  }
  if (!pixel(output, 3.1, 10, 10, &red, &green, &blue) || blue < 150 || red > 80) {
    std::cerr << "Wrong clip after card\n";
    return 1;
  }
  QByteArray pcm;
  if (!run(
          "ffmpeg",
          {"-v",
           "error",
           "-ss",
           "2.15",
           "-t",
           "0.4",
           "-i",
           output,
           "-vn",
           "-ac",
           "1",
           "-ar",
           "48000",
           "-f",
           "s16le",
           "pipe:1"},
          &pcm))
    return 1;
  qint64 energy = 0;
  for (int i = 0; i + 1 < pcm.size(); i += 2)
    energy += std::abs(qint16(quint8(pcm[i]) | (quint16(quint8(pcm[i + 1])) << 8)));
  if (pcm.isEmpty() || energy / (pcm.size() / 2) > 20) {
    std::cerr << "Card audio was not silent\n";
    return 1;
  }
  // MP4 empty edits preserve delayed audio in a segment even when decoded PTS
  // begins at zero. It must stay silent before its actual archive start time.
  const QString delayed_source = dir.filePath("delayed-audio.mp4");
  if (!run(
          "ffmpeg",
          {"-v",
           "error",
           "-f",
           "lavfi",
           "-i",
           "color=c=blue:s=640x360:r=30:d=4",
           "-itsoffset",
           "2",
           "-f",
           "lavfi",
           "-i",
           "sine=frequency=440:sample_rate=48000:duration=2",
           "-c:v",
           "libx264",
           "-g",
           "30",
           "-c:a",
           "aac",
           delayed_source}))
    return 1;
  auto delayed = r;
  delayed.archive_path = delayed_source;
  delayed.output_path = dir.filePath("delayed-reel.mp4");
  auto before_audio = clip, during_audio = clip;
  before_audio.start_ms = 10000;
  before_audio.end_ms = 11000;
  before_audio.annotations.clear();
  during_audio.start_ms = 12500;
  during_audio.end_ms = 13500;
  during_audio.annotations.clear();
  delayed.items = {before_audio, card, during_audio};
  if (!pipeline.Start(delayed, &error) || !wait(pipeline))
    return 1;
  const qint64 silent_level = audio_level(delayed.output_path, .1, .7);
  if (silent_level < 0 || silent_level > 20 || audio_level(delayed.output_path, 2, .5) < 500) {
    std::cerr << "MP4 edit-list audio lost its archive time\n";
    return 1;
  }
  const QString transients = dir.filePath("transients.mkv");
  if (!run(
          "ffmpeg",
          {"-v",
           "error",
           "-f",
           "lavfi",
           "-i",
           "color=c=blue:s=640x360:r=30:d=4",
           "-f",
           "lavfi",
           "-i",
           "aevalsrc=0.8*sin(2*PI*1000*t)*(between(t\\,2.45\\,2.55)+between(t\\,3.125\\,3.145)):s=48000:d=4",
           "-c:v",
           "libx264",
           "-g",
           "30",
           "-c:a",
           "pcm_s16le",
           transients}))
    return 1;
  auto transient = r;
  transient.archive_path = transients;
  transient.output_path = dir.filePath("transient-reel.mp4");
  auto transient_clip = before_audio;
  transient_clip.start_ms = 12250;
  transient_clip.end_ms = 13150;
  transient.items = {transient_clip};
  if (!pipeline.Start(transient, &error) || !wait(pipeline) || !transient_span(transient.output_path, 200, 895))
    return 1;
  // AAC preroll must remain encoded for overlap state. Dropping its packet
  // instead of editing presentation attenuates the first 20 ms of this cut.
  transient_clip.start_ms = 12450;
  transient_clip.end_ms = 12550;
  transient.items = {transient_clip};
  transient.output_path = dir.filePath("transient-start.mp4");
  if (!pipeline.Start(transient, &error) || !wait(pipeline))
    return 1;
  const qint64 start_level = audio_level(transient.output_path, 0, .02);
  if (start_level < 10000) {
    std::cerr << "AAC preroll lost initial transient samples: level=" << start_level << '\n';
    return 1;
  }
  const QString source10 = dir.filePath("source10.mp4");
  if (!run(
          "ffmpeg",
          {"-v",
           "error",
           "-f",
           "lavfi",
           "-i",
           "color=c=blue:s=640x360:r=30:d=2",
           "-vf",
           "format=yuv420p10le",
           "-c:v",
           "libx265",
           "-x265-params",
           "pools=1:frame-threads=1:log-level=error",
           "-pix_fmt",
           "yuv420p10le",
           source10}))
    return 1;
  auto high = r;
  high.archive_path = source10;
  high.output_path = dir.filePath("high10.mp4");
  high.media.bit_depth = 10;
  high.media.video_codec = "hevc";
  high.media.has_audio = false;
  auto short_clip = clip;
  short_clip.start_ms = 10500;
  short_clip.end_ms = 11500;
  high.items = {card, short_clip};
  if (!pipeline.Start(high, &error) || !wait(pipeline))
    return 1;
  QByteArray depth;
  if (!run(
          "ffprobe",
          {"-v",
           "error",
           "-select_streams",
           "v:0",
           "-show_entries",
           "stream=pix_fmt",
           "-of",
           "default=nw=1:nk=1",
           high.output_path},
          &depth) ||
      !depth.contains("10")) {
    std::cerr << "Lost ten-bit export " << depth.constData();
    return 1;
  }
  // Cards work with no archive, and cancellation unblocks bounded queues.
  r.archive_path.clear();
  r.items = {card};
  r.output_path = dir.filePath("card.mp4");
  if (!pipeline.Start(r, &error) || !wait(pipeline))
    return 1;
  // A panorama card must keep a square logo square in the encoded result.
  QImage logo(80, 80, QImage::Format_RGBA8888);
  logo.fill(Qt::green);
  const QString logo_path = dir.filePath("logo.png");
  if (!logo.save(logo_path))
    return 1;
  auto panorama = r;
  panorama.media.width = 1280;
  panorama.media.height = 320;
  panorama.items[0].card.matchup = true;
  panorama.items[0].card.team_a = "Home";
  panorama.items[0].card.team_b = "Away";
  panorama.items[0].card.date = "2026-10-08";
  panorama.items[0].card.logo_a = logo_path;
  panorama.output_path = dir.filePath("panorama-card.mp4");
  if (!pipeline.Start(panorama, &error) || !wait(pipeline) ||
      !pixel(panorama.output_path, .2, 470, 110, &red, &green, &blue) || green < 150 || red > 80 ||
      !pixel(panorama.output_path, .2, 540, 180, &red, &green, &blue) || green < 150 || red > 80 ||
      !pixel(panorama.output_path, .2, 550, 110, &red, &green, &blue) || red < 150 || green > 80) {
    std::cerr << "Panorama card distorted or misplaced the logo\n";
    return 1;
  }
  card.card.duration_ms = 600000;
  r.items = {card};
  r.output_path = dir.filePath("cancel.mp4");
  if (!pipeline.Start(r, &error))
    return 1;
  QThread::msleep(100);
  pipeline.Cancel();
  if (!wait(pipeline, 5000, true))
    return 1;
  // Native preview, inspection and resize share exactly the export renderer.
  if (QGuiApplication::platformName() == "xcb") {
    QWidget window;
    window.setAttribute(Qt::WA_NativeWindow);
    window.resize(640, 360);
    window.show();
    app->processEvents();
    r.archive_path = archive;
    r.items = {clip};
    r.output_path.clear();
    r.window_id = window.winId();
    r.inspect_game_ms = 10750;
    if (!pipeline.Start(r, &error) || !wait(pipeline))
      return 1;
    window.resize(800, 450);
    app->processEvents();
    pipeline.Cancel();
    r.inspect_game_ms = -1;
    r.items = {clip, second};
    r.loop = true;
    if (!pipeline.Start(r, &error))
      return 1;
    for (int i = 0; i < 80; ++i) {
      app->processEvents();
      QThread::msleep(10);
    }
    pipeline.Cancel();
    if (!wait(pipeline, 5000, true))
      return 1;
  }
  return 0;
}
