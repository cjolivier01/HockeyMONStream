#include "hstream/src/libs/media/Mp4VideoTrack.h"
#include "hstream/src/libs/media/TestMp4.h"

#include <fstream>
#include <limits>

int main(int argc, char** argv) {
  using namespace hm::media;
  using test::Check;
  try {
    if (argc == 2) {
      const auto info = ReadMp4VideoTrack(argv[1], 10000000000, {});
      Check(info.ok(), info.status().ToString());
      std::cout << info->width << 'x' << info->height << ' ' << info->fps_n << '/' << info->fps_d
                << " frames=" << info->frames << " start=" << info->start_ns << " duration=" << info->duration_ns
                << '\n';
      return 0;
    }
    test::Directory root;
    const auto ordinary = root.path / "ordinary.mp4", offset = root.path / "offset.mp4";
    test::Chapter(ordinary, "black", "0");
    test::Chapter(offset, "white", "0.4");
    for (const auto& path : {ordinary, offset}) {
      const auto info = ReadMp4VideoTrack(path, std::numeric_limits<int64_t>::max(), {});
      Check(info.ok(), info.status().ToString());
      Check(
          info->codec == VideoCodec::kH264 && info->width == 384 && info->height == 192 && info->fps_n == 25 &&
              info->fps_d == 1 && info->frames == 5 && info->duration_ns == 200000000 &&
              info->start_ns == (path == offset ? 400000000 : 0),
          "Video track inherited an audio tail or lost its start/fps/frame count");
      Check(info->bitrate && *info->bitrate > 0, "MP4 video header bitrate was lost");
    }
    Check(
        absl::IsCancelled(ReadMp4VideoTrack(ordinary, 1000000000, [] { return true; }).status()),
        "Cancellation was ignored");
    Check(
        absl::IsInternal(
            ReadMp4VideoTrack(ordinary, 1000000000, []() -> bool { throw std::runtime_error("cancel"); }).status()),
        "Throwing callback escaped through FFmpeg");
    Check(absl::IsDeadlineExceeded(ReadMp4VideoTrack(ordinary, 1, {}).status()), "Expired deadline accepted");
    Check(absl::IsInvalidArgument(ReadMp4VideoTrack(ordinary, 0, {}).status()), "Zero deadline accepted");
    Check(absl::IsNotFound(ReadMp4VideoTrack(root.path / "missing", 1000000000, {}).status()), "Missing file accepted");
    const auto bad = root.path / "bad.mp4";
    std::ofstream(bad) << "not an MP4";
    Check(!ReadMp4VideoTrack(bad, 1000000000, {}).ok(), "Malformed MP4 accepted");
    const auto audio = root.path / "audio.mp4";
    test::Ffmpeg({"-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo:d=0.1", "-c:a", "aac", audio.string()});
    Check(absl::IsInvalidArgument(ReadMp4VideoTrack(audio, 1000000000, {}).status()), "Audio-only MP4 accepted");
    const auto duplicate = root.path / "two-videos.mp4";
    test::Ffmpeg({"-i", ordinary.string(), "-map", "0:v", "-map", "0:v", "-c", "copy", duplicate.string()});
    Check(
        absl::IsInvalidArgument(ReadMp4VideoTrack(duplicate, 1000000000, {}).status()),
        "Ambiguous video tracks accepted");
    std::cout << "MP4 video-only clock, start offset, metadata and error checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
