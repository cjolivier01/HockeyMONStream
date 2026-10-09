#include "src/apps/hstream-ui/HighlightMp4Audio.h"
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QtEndian>
#include <iostream>

namespace {
void put(QByteArray& bytes, int at, int width, quint64 value) {
  if (width == 8)
    qToBigEndian<quint64>(value, bytes.data() + at);
  else
    qToBigEndian<quint32>(value, bytes.data() + at);
}
QByteArray box(const char* type, QByteArray bytes, bool extended = false) {
  QByteArray result(extended ? 16 : 8, '\0');
  put(result, 0, 4, extended ? 1 : result.size() + bytes.size());
  result.replace(4, 4, type);
  if (extended)
    put(result, 8, 8, result.size() + bytes.size());
  return result + bytes;
}
QByteArray time_header(const char* type, int version, quint32 scale, quint64 duration) {
  QByteArray bytes(version ? 40 : 28, '\0');
  bytes[0] = char(version);
  put(bytes, version ? 20 : 12, 4, scale);
  put(bytes, version ? 24 : 16, version ? 8 : 4, duration);
  return box(type, bytes);
}
QByteArray track(int version, bool audio, quint32 rate = 48000) {
  QByteArray tkhd(version ? 40 : 32, '\0');
  tkhd[0] = char(version);
  put(tkhd, version ? 28 : 20, version ? 8 : 4, 2000);
  QByteArray hdlr(16, '\0');
  hdlr.replace(8, 4, audio ? "soun" : "vide");
  QByteArray elst(version ? 32 : 20, '\0');
  elst[0] = char(version);
  put(elst, 4, 4, 1);
  put(elst, 8, version ? 8 : 4, 2000);
  put(elst, version ? 24 : 16, 4, 65536);
  return box(
      "trak",
      box("tkhd", tkhd) + box("mdia", box("hdlr", hdlr) + time_header("mdhd", version, rate, 96000)) +
          box("edts", box("elst", elst)));
}
bool write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return file.readAll();
}
} // namespace

int main() {
  QTemporaryDir directory;
  const QString path = directory.filePath("reel.mp4");
  QString error;
  for (int version : {0, 1}) {
    const QByteArray video = track(version, false), audio = track(version, true);
    const QByteArray media = box("mdat", "encoded data stays unchanged", true);
    const QByteArray movie = box("moov", time_header("mvhd", version, 1000, 2000) + video + audio);
    if (!write(path, media + movie) || !hm::ui::FinalizeHighlightMp4Audio(path, 1000, &error)) {
      std::cerr << error.toStdString() << '\n';
      return 1;
    }
    const auto edited = read(path);
    const int list = edited.lastIndexOf("elst") + 4;
    const quint64 start = version ? qFromBigEndian<quint64>(edited.constData() + list + 16)
                                  : qFromBigEndian<quint32>(edited.constData() + list + 12);
    if (edited.size() != media.size() + movie.size() || !edited.startsWith(media) || !edited.contains(video) ||
        start != 1024)
      return 1;
  }
  const QByteArray movie = time_header("mvhd", 0, 1000, 2000) + track(0, false);
  // Unexpected structures must fail before any metadata changes reach disk.
  for (const QByteArray& bytes :
       {box("moov", movie + track(0, true, 44100)),
        box("moov", movie + track(0, true) + track(0, true)),
        box("moov", movie + track(0, true)).chopped(1)}) {
    if (!write(path, bytes) || hm::ui::FinalizeHighlightMp4Audio(path, 1000, &error) || read(path) != bytes)
      return 1;
  }
  return 0;
}
