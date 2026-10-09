#include "src/apps/hstream-ui/HighlightReelPipeline.h"
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
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
  card.card.duration_ms = 750;
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
      std::abs(duration.trimmed().toDouble() - 4.2) > .08) {
    std::cerr << "Bad mixed duration " << duration.constData();
    return 1;
  }
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
