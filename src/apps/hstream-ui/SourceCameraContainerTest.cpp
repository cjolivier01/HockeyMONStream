#include "hstream/src/libs/media/TestMp4.h"
#include "src/apps/hstream-ui/NativePreviewSurface.h"
#include "src/apps/hstream-ui/SourceCameraPlayer.h"
#include "src/apps/hstream-ui/SourceCameraTrack.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtWidgets/QApplication>
#include <cmath>

namespace {
using hm::media::test::Check;
template <class Predicate>
hm::ui::SourceCameraPlayer::Status Wait(hm::ui::SourceCameraPlayer& player, Predicate predicate) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 15000) {
    QCoreApplication::processEvents();
    const auto status = player.Poll();
    Check(status.error.isEmpty(), status.error.toStdString());
    if (predicate(status))
      return status;
    QThread::msleep(10);
  }
  throw std::runtime_error("Camera container playback/seek timed out");
}
} // namespace

int main(int argc, char** argv) {
  try {
    hm::media::test::Directory directory;
    QApplication application(argc, argv);
    hm::ui::NativePreviewSurface surface;
    surface.setAttribute(Qt::WA_NativeWindow);
    surface.setAttribute(Qt::WA_PaintOnScreen);
    surface.setAttribute(Qt::WA_NoSystemBackground);
    surface.resize(640, 360);
    surface.show();
    QCoreApplication::processEvents();
    for (const char* extension : {"mp4", "mkv", "avi"}) {
      const auto path = directory.path / (std::string("camera.") + extension);
      const bool mp4 = std::string(extension) == "mp4";
      const bool mkv = std::string(extension) == "mkv";
      // MP4 edit lists and Matroska normalize nonzero timestamps differently.
      // Preserve typical H.264 B-frames and a longer audio tail in both cases.
      hm::media::test::Ffmpeg(
          {"-f",
           "lavfi",
           "-i",
           "testsrc2=s=640x360:r=25:d=3",
           "-f",
           "lavfi",
           "-i",
           "anullsrc=r=48000:cl=stereo:d=5",
           "-c:v",
           "libx264",
           "-g",
           "25",
           "-c:a",
           mp4 ? "aac" : "pcm_s16le",
           "-output_ts_offset",
           (mp4 || mkv) ? "2" : "0",
           path.string()});
      const auto metadata = hm::ui::ReadSourceCameraTrack(QString::fromStdString(path.string()), 15000);
      Check(metadata.ok(), metadata.status().ToString());
      Check(std::abs(metadata->duration_ns - 3000000000LL) < 80000000, "Metadata retained an audio tail");
      if (mp4 || mkv)
        Check(std::abs(metadata->start_ns - 2000000000LL) < 80000000, "Metadata lost the physical video origin");
      const auto cancelled =
          hm::ui::ReadSourceCameraTrack(QString::fromStdString(path.string()), 15000, [] { return true; });
      Check(absl::IsCancelled(cancelled.status()), "Metadata ignored cancellation");
      hm::ui::SourceCameraPlayer player;
      QString error;
      Check(player.Open({QString::fromStdString(path.string())}, surface.winId(), 15000, &error), error.toStdString());
      auto status = Wait(player, [](const auto& value) { return value.presented_ns >= 0; });
      Check(std::abs(status.duration_ms - 3000) < 80, "Audio tail changed the video timeline");
      // Packet PTS differ from the demuxer's stream-time clock. Anchor expected
      // displayed frames to the first decoded frame, not the container offset.
      const auto first_pts = status.presented_ns;
      player.Play();
      status = Wait(player, [](const auto& value) { return value.position_ms > 250; });
      Check(status.position_ms < 1000, "Camera position retained the container timestamp offset");
      player.Pause();
      player.Seek(1200);
      status = Wait(player, [first_pts](const auto& value) {
        return std::abs(value.presented_ns - first_pts - 1200000000LL) < 80000000;
      });
      Check(!status.playing && std::abs(status.position_ms - 1200) < 80, "Paused seek changed the playback state");
      // UI tests inspect retained pixels. Exercise the direct renderer path here
      // after the native surface is remapped, without changing the seek position.
      surface.hide();
      surface.show();
      QCoreApplication::processEvents();
      Check(player.Redraw(), "Paused GPU sample was unavailable for redraw");
      Check(!player.GetStatus().playing, "Redrawing the retained frame started playback");
      player.Seek(2900);
      status = Wait(player, [first_pts](const auto& value) { return value.presented_ns - first_pts >= 2880000000LL; });
      Check(status.position_ms > 2800, "Offset recording lost its final video frames");
      QElapsedTimer tail;
      tail.start();
      player.Play();
      Wait(player, [](const auto& value) { return !value.playing && value.position_ms == value.duration_ms; });
      Check(tail.elapsed() < 1500, "Playback waited for the audio tail after the video chapter ended");
      player.Close();
      Check(!player.GetStatus().open, "Close retained the camera decoder");
      std::cout << extension << " hardware playback and paused seek passed\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
