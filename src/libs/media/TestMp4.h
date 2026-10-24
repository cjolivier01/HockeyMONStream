#pragma once

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "hstream/src/libs/common/Process.h"

namespace hm::media::test {
inline void Check(bool ok, const std::string& message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Directory {
  std::filesystem::path path;
  Directory() {
    const char* root = std::getenv("TEST_TMPDIR");
    auto name =
        ((root ? std::filesystem::path(root) : std::filesystem::temp_directory_path()) / "hstream-mp4-XXXXXX").string();
    Check(mkdtemp(name.data()), "Cannot create test directory");
    path = name;
  }
  ~Directory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    if (error)
      std::cerr << "MP4 test cleanup: " << error.message() << '\n';
  }
};
inline void Ffmpeg(std::vector<std::string> arguments) {
  // Only synthetic fixture preparation uses software encoding. Production
  // chapter discovery neither launches a process nor opens a video codec.
  arguments.insert(arguments.begin(), {"/usr/bin/ffmpeg", "-nostdin", "-v", "error"});
  std::string errors;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  const int status = hm::run_command(
      arguments,
      {},
      {},
      [&](const auto& err, const auto& out) { errors += err + out; },
      [&] { return std::chrono::steady_clock::now() >= deadline; });
  Check(status == 0, "Cannot generate MP4 fixture: " + errors);
}
// Keep video above qtdemux's 20% thumbnail heuristic, including its start offset.
inline void Chapter(const std::filesystem::path& path, const char* color, const char* start) {
  Ffmpeg(
      {"-f",
       "lavfi",
       "-i",
       std::string("color=c=") + color + ":s=384x192:r=25:d=0.2",
       "-f",
       "lavfi",
       "-i",
       "anullsrc=r=48000:cl=stereo:d=0.5",
       "-map",
       "0:v",
       "-map",
       "1:a",
       "-c:v",
       "libx264",
       "-bf",
       "2",
       "-pix_fmt",
       "yuv420p",
       "-color_range",
       "tv",
       "-colorspace",
       "bt709",
       "-color_primaries",
       "bt709",
       "-color_trc",
       "bt709",
       "-c:a",
       "aac",
       "-output_ts_offset",
       start,
       path.string()});
}
} // namespace hm::media::test
