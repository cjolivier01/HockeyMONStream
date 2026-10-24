#include "hstream/src/libs/media/Mp4AudioEdit.h"
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace hm::media {
namespace {
struct Box {
  int64_t payload, end;
  std::string type;
};
struct Write {
  int64_t offset;
  std::string bytes;
};
class Edits {
 public:
  int fd;
  int64_t size;
  std::vector<Write> writes;
  explicit Edits(int descriptor) : fd(descriptor) {
    struct stat file {};
    if (fstat(fd, &file) || !S_ISREG(file.st_mode) || file.st_size <= 0)
      throw std::runtime_error("MP4 audio edit requires a completed regular file");
    size = file.st_size;
  }
  std::string read(int64_t offset, int64_t count, int64_t end) {
    if (offset < 0 || count < 0 || offset > end || count > end - offset)
      throw std::runtime_error("Invalid MP4 metadata bounds");
    std::string bytes(count, '\0');
    size_t done = 0;
    while (done < bytes.size()) {
      const auto n = pread(fd, bytes.data() + done, bytes.size() - done, offset + done);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        throw std::runtime_error("Incomplete MP4 metadata");
      done += n;
    }
    return bytes;
  }
  uint64_t number(int64_t offset, int width, int64_t end) {
    const auto bytes = read(offset, width, end);
    uint64_t value = 0;
    for (const unsigned char byte : bytes)
      value = (value << 8) | byte;
    return value;
  }
  std::vector<Box> children(int64_t start, int64_t end) {
    std::vector<Box> result;
    while (start < end) {
      if (result.size() >= 4096)
        throw std::runtime_error("Too many MP4 metadata boxes");
      const auto header = read(start, 8, end);
      uint64_t size = number(start, 4, end);
      int header_size = 8;
      if (size == 1) {
        size = number(start + 8, 8, end);
        header_size = 16;
      } else if (size == 0)
        size = end - start;
      if (size < uint64_t(header_size) || size > uint64_t(end - start))
        throw std::runtime_error("Malformed MP4 metadata box");
      result.push_back({start + header_size, start + int64_t(size), header.substr(4, 4)});
      start += size;
    }
    return result;
  }
  Box child(const Box& parent, const std::string& type) {
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
      throw std::runtime_error("Required MP4 metadata box is missing: " + type);
    return result;
  }
  int version(const Box& box) {
    const int v = uint8_t(read(box.payload, 1, box.end)[0]);
    if (v > 1)
      throw std::runtime_error("Unsupported MP4 metadata version");
    return v;
  }
  void write(int64_t offset, int width, uint64_t value, int64_t end) {
    read(offset, width, end); // validate every edit before modifying the file
    if (width == 4 && value > std::numeric_limits<uint32_t>::max())
      throw std::runtime_error("MP4 metadata duration overflow");
    std::string bytes(width, '\0');
    for (int i = width - 1; i >= 0; --i) {
      bytes[i] = value & 255;
      value >>= 8;
    }
    writes.push_back({offset, bytes});
  }
  void commit() {
    for (const auto& write : writes) {
      size_t done = 0;
      while (done < write.bytes.size()) {
        const auto n = pwrite(fd, write.bytes.data() + done, write.bytes.size() - done, write.offset + done);
        if (n < 0 && errno == EINTR)
          continue;
        if (n <= 0)
          throw std::runtime_error("Could not update MP4 audio edit");
        done += n;
      }
    }
    // Publication owns fsync; these positional writes preserve the mux offset.
  }
};
} // namespace

absl::Status FinalizeMp4Audio(int fd, int sample_rate, int64_t samples) {
  try {
    if (samples <= 0 || sample_rate <= 0)
      throw std::runtime_error("Invalid MP4 audio duration");
    Edits edits(fd);
    const Box moov = edits.child({0, edits.size, {}}, "moov");
    const Box movie = edits.child(moov, "mvhd");
    const int mv = edits.version(movie);
    const uint64_t scale = edits.number(movie.payload + (mv ? 20 : 12), 4, movie.end);
    const auto scaled = (static_cast<__int128>(samples) * scale + sample_rate / 2) / sample_rate;
    if (!scale || scaled <= 0 || scaled > INT64_MAX)
      throw std::runtime_error("Invalid MP4 movie timescale or duration");
    const uint64_t duration = scaled;
    uint64_t movie_duration = duration;
    int audio_tracks = 0;
    for (const auto& track : edits.children(moov.payload, moov.end)) {
      if (track.type != "trak")
        continue;
      const Box tkhd = edits.child(track, "tkhd"), mdia = edits.child(track, "mdia");
      const Box handler = edits.child(mdia, "hdlr");
      const int tv = edits.version(tkhd);
      const int64_t track_duration = tkhd.payload + (tv ? 28 : 20);
      if (edits.read(handler.payload + 8, 4, handler.end) != "soun") {
        movie_duration = std::max(movie_duration, edits.number(track_duration, tv ? 8 : 4, tkhd.end));
        continue;
      }
      ++audio_tracks;
      const Box media = edits.child(mdia, "mdhd");
      const int av = edits.version(media);
      if (edits.number(media.payload + (av ? 20 : 12), 4, media.end) != uint64_t(sample_rate))
        throw std::runtime_error("Unexpected MP4 audio sample rate");
      const Box list = edits.child(edits.child(track, "edts"), "elst");
      const int ev = edits.version(list);
      if (edits.number(list.payload + 4, 4, list.end) != 1 ||
          edits.number(list.payload + (ev ? 16 : 12), ev ? 8 : 4, list.end) != 0)
        throw std::runtime_error("Unexpected MP4 audio edit list");
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
    return absl::OkStatus();
  } catch (const std::exception& e) {
    return absl::DataLossError(e.what());
  }
}
} // namespace hm::media
