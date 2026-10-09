#include "src/apps/hstream-ui/HighlightMp4Audio.h"
#include <QtCore/QFile>
#include <QtCore/QtEndian>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

namespace hm::ui {
namespace {
struct Box {
  qint64 payload, end;
  QByteArray type;
};
struct Write {
  qint64 offset;
  QByteArray bytes;
};
class Edits {
 public:
  QFile file;
  std::vector<Write> writes;
  explicit Edits(const QString& path) : file(path) {
    if (!file.open(QIODevice::ReadWrite))
      throw std::runtime_error(file.errorString().toStdString());
  }
  QByteArray read(qint64 offset, qint64 count, qint64 end) {
    if (offset < 0 || count < 0 || offset > end || count > end - offset || !file.seek(offset))
      throw std::runtime_error("Invalid MP4 metadata bounds");
    const auto bytes = file.read(count);
    if (bytes.size() != count)
      throw std::runtime_error("Incomplete MP4 metadata");
    return bytes;
  }
  quint64 number(qint64 offset, int width, qint64 end) {
    const auto bytes = read(offset, width, end);
    return width == 8 ? qFromBigEndian<quint64>(bytes.constData()) : qFromBigEndian<quint32>(bytes.constData());
  }
  std::vector<Box> children(qint64 start, qint64 end) {
    std::vector<Box> result;
    while (start < end) {
      if (result.size() >= 4096)
        throw std::runtime_error("Too many MP4 metadata boxes");
      const auto header = read(start, 8, end);
      quint64 size = qFromBigEndian<quint32>(header.constData());
      int header_size = 8;
      if (size == 1) {
        size = number(start + 8, 8, end);
        header_size = 16;
      } else if (size == 0)
        size = end - start;
      if (size < quint64(header_size) || size > quint64(end - start))
        throw std::runtime_error("Malformed MP4 metadata box");
      result.push_back({start + header_size, start + qint64(size), header.mid(4, 4)});
      start += size;
    }
    return result;
  }
  Box child(const Box& parent, const QByteArray& type) {
    Box result{};
    bool found = false;
    for (const auto& box : children(parent.payload, parent.end))
      if (box.type == type) {
        if (found)
          throw std::runtime_error("Duplicate MP4 metadata box");
        result = box;
        found = true;
      }
    if (!found)
      throw std::runtime_error("Required MP4 metadata box is missing: " + type.toStdString());
    return result;
  }
  int version(const Box& box) {
    const int v = quint8(read(box.payload, 1, box.end)[0]);
    if (v > 1)
      throw std::runtime_error("Unsupported MP4 metadata version");
    return v;
  }
  void write(qint64 offset, int width, quint64 value, qint64 end) {
    read(offset, width, end); // validate every edit before modifying the file
    if (width == 4 && value > std::numeric_limits<quint32>::max())
      throw std::runtime_error("MP4 metadata duration overflow");
    QByteArray bytes(width, '\0');
    if (width == 8)
      qToBigEndian<quint64>(value, bytes.data());
    else
      qToBigEndian<quint32>(value, bytes.data());
    writes.push_back({offset, bytes});
  }
  void commit() {
    for (const auto& write : writes)
      if (!file.seek(write.offset) || file.write(write.bytes) != write.bytes.size())
        throw std::runtime_error("Could not update MP4 audio edit");
    if (!file.flush())
      throw std::runtime_error("Could not flush MP4 audio edit");
  }
};
} // namespace

bool FinalizeHighlightMp4Audio(const QString& path, qint64 duration_ms, QString* error) {
  try {
    if (duration_ms <= 0)
      throw std::runtime_error("Invalid reel audio duration");
    Edits edits(path);
    const Box moov = edits.child({0, edits.file.size(), {}}, "moov");
    const Box movie = edits.child(moov, "mvhd");
    const int mv = edits.version(movie);
    const quint64 scale = edits.number(movie.payload + (mv ? 20 : 12), 4, movie.end);
    if (!scale || quint64(duration_ms / 1000) > std::numeric_limits<quint64>::max() / scale)
      throw std::runtime_error("Invalid MP4 movie timescale or duration");
    const quint64 whole = quint64(duration_ms / 1000) * scale;
    const quint64 fraction = (quint64(duration_ms % 1000) * scale + 500) / 1000;
    if (whole > std::numeric_limits<quint64>::max() - fraction)
      throw std::runtime_error("MP4 duration overflow");
    const quint64 duration = whole + fraction;
    quint64 movie_duration = duration;
    int audio_tracks = 0;
    for (const auto& track : edits.children(moov.payload, moov.end)) {
      if (track.type != "trak")
        continue;
      const Box tkhd = edits.child(track, "tkhd"), mdia = edits.child(track, "mdia");
      const Box handler = edits.child(mdia, "hdlr");
      const int tv = edits.version(tkhd);
      const qint64 track_duration = tkhd.payload + (tv ? 28 : 20);
      if (edits.read(handler.payload + 8, 4, handler.end) != "soun") {
        movie_duration = std::max(movie_duration, edits.number(track_duration, tv ? 8 : 4, tkhd.end));
        continue;
      }
      ++audio_tracks;
      const Box media = edits.child(mdia, "mdhd");
      const int av = edits.version(media);
      if (edits.number(media.payload + (av ? 20 : 12), 4, media.end) != 48000)
        throw std::runtime_error("Unexpected reel audio sample rate");
      const Box list = edits.child(edits.child(track, "edts"), "elst");
      const int ev = edits.version(list);
      if (edits.number(list.payload + 4, 4, list.end) != 1 ||
          edits.number(list.payload + (ev ? 16 : 12), ev ? 8 : 4, list.end) != 0)
        throw std::runtime_error("Unexpected reel audio edit list");
      // avenc_aac AAC-LC has one 1024-sample priming frame. Keep the encoded
      // frame so the decoder retains overlap state, and skip it via the edit.
      // No box sizes or media/chunk offsets change; only fixed metadata fields.
      edits.write(list.payload + 8, ev ? 8 : 4, duration, list.end);
      edits.write(list.payload + (ev ? 16 : 12), ev ? 8 : 4, 1024, list.end);
      edits.write(track_duration, tv ? 8 : 4, duration, tkhd.end);
    }
    if (audio_tracks != 1)
      throw std::runtime_error("Expected one reel audio track");
    edits.write(movie.payload + (mv ? 24 : 16), mv ? 8 : 4, movie_duration, movie.end);
    edits.commit();
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = QString::fromUtf8(e.what());
    return false;
  }
}
} // namespace hm::ui
