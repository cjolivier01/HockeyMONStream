#include "hstream/src/libs/media/VideoStill.h"
#include "hstream/src/libs/media/TestMp4.h"

int main() {
  using hm::media::ReadVideoStill;
  using hm::media::test::Check;
  try {
    hm::media::test::Directory directory;
    const auto path = directory.path / "still.mp4";
    hm::media::test::Ffmpeg(
        {"-f", "lavfi", "-i", "testsrc2=s=384x216:r=25:d=1", "-c:v", "libx264", "-bf", "2", path.string()});
    auto read = [&](int64_t time, int limit, const std::function<bool()>& cancel = {}) {
      return ReadVideoStill(path.string(), false, time, limit, 67108864, 5000000000, cancel);
    };
    auto first = read(0, 192);
    Check(first.ok(), first.status().ToString());
    Check(
        first->source_width == 384 && first->source_height == 216 && first->width == 192 && first->height == 108 &&
            first->rgba.size() == 192 * 108 * 4 && first->time_ns == 0,
        "Preview lost its original geometry or exceeded its proxy bounds");
    auto next = read(400000000, 192);
    Check(
        next.ok() && next->time_ns == 400000000 && next->rgba != first->rgba,
        "Preview did not seek to the selected source frame");
    Check(!read(2000000000, 192).ok(), "Preview reused a frame beyond EOS");
    Check(absl::IsCancelled(read(0, 192, [] { return true; }).status()), "Preview ignored cancellation");
    Check(!read(-1, 192).ok() && !read(0, 0).ok(), "Invalid preview bounds accepted");
    Check(!ReadVideoStill(path.string(), false, 0, 192, 16, 5000000000, {}).ok(), "Oversized source accepted");
    Check(
        !ReadVideoStill("https://example.com/video", true, 0, 192, 67108864, 5000000000, {}).ok(),
        "Live preview accepted an unsupported protocol");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
