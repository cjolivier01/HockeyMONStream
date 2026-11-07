#include "hstream/src/libs/media/TestMp4.h"
#include "src/apps/hstream-ui/NativePreviewSurface.h"
#include "src/apps/hstream-ui/SourceCameraPlayer.h"

#include <QtGui/qguiapplication_platform.h>
#include <xcb/xcb.h>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtGui/QImage>
#include <QtWidgets/QApplication>
#include <algorithm>
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
  throw std::runtime_error("Camera pixel regression timed out");
}

// Bounded test-only front-buffer readback, never part of camera playback.
QImage Presented(hm::ui::NativePreviewSurface& surface) {
  auto* connection = qGuiApp->nativeInterface<QNativeInterface::QX11Application>()->connection();
  auto* reply = xcb_get_image_reply(
      connection, xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, surface.winId(), 0, 0, 640, 360, ~0U), nullptr);
  Check(reply && xcb_get_image_data_length(reply) == 640 * 360 * 4, "Cannot read the displayed camera frame");
  QImage image(xcb_get_image_data(reply), 640, 360, QImage::Format_RGB32);
  image = image.copy();
  std::free(reply);
  return image;
}

void Compare(const QImage& actual, const QImage& expected) {
  Check(actual.size() == expected.size(), "Reference frame dimensions differ");
  if (const char* capture = std::getenv("HSTREAM_TEST_CAMERA_CAPTURE"))
    Check(actual.save(QString::fromLocal8Bit(capture)), "Cannot save requested test screenshot");
  // Check each vertical strip independently: an overall average can hide stale
  // right-hand slices when most of a recording is stationary ice/background.
  for (int strip = 0; strip < 8; ++strip) {
    double error = 0;
    int samples = 0;
    for (int y = 2; y < 358; ++y)
      for (int x = std::max(2, strip * 80); x < std::min(638, (strip + 1) * 80); ++x) {
        const auto a = actual.pixelColor(x, y), b = expected.pixelColor(x, y);
        // GL samples the full-size texture; FFmpeg antialiases its reduction.
        // Exclude textured reference edges where these intentionally differ.
        // Smooth ice still exposes stale players, and the changing-color
        // fixture exercises broad regions across every decoder slice.
        bool smooth = true;
        for (int dy : {-2, 0, 2})
          for (int dx : {-2, 0, 2}) {
            const auto neighbor = expected.pixelColor(x + dx, y + dy);
            if (std::abs(b.red() - neighbor.red()) > 12 || std::abs(b.green() - neighbor.green()) > 12 ||
                std::abs(b.blue() - neighbor.blue()) > 12)
              smooth = false;
          }
        if (!smooth)
          continue;
        error += std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue());
        ++samples;
      }
    Check(samples > 1000, "Too few smooth reference pixels in strip " + std::to_string(strip));
    const double mean = error / (samples * 3);
    std::cout << "strip " << strip << " mean pixel error " << mean << '\n';
    Check(mean < 4.0, "Camera contains stale or incorrect pixels in strip " + std::to_string(strip));
  }
}
} // namespace

int main(int argc, char** argv) {
  try {
    hm::media::test::Directory directory;
    QApplication application(argc, argv);
    const bool supplied = argc == 3;
    Check(argc == 1 || supplied, "Usage: source_camera_seek_pixels_test [recording chapter-local-seconds]");
    const auto source = supplied ? std::filesystem::path(argv[1]) : directory.path / "camera.mp4";
    const qint64 target = supplied ? std::llround(std::stod(argv[2]) * 1000) : 1000;
    if (!supplied)
      hm::media::test::Ffmpeg(
          {"-f",
           "lavfi",
           "-i",
           "testsrc2=s=640x360:r=6:d=2,hue=h=n*40,scale=7680:4320:flags=neighbor",
           "-c:v",
           "libx265",
           "-preset",
           "ultrafast",
           "-x265-params",
           "pools=4:frame-threads=2:log-level=error",
           "-pix_fmt",
           "yuv420p10le",
           "-g",
           "6",
           "-bf",
           "2",
           source.string()});
    const auto reference = directory.path / "reference.png";
    hm::media::test::Ffmpeg(
        {"-threads",
         "4",
         "-ss",
         std::to_string(target / 1000.0),
         "-i",
         source.string(),
         "-map",
         "0:v:0",
         "-frames:v",
         "1",
         "-vf",
         "scale=640:360",
         "-update",
         "1",
         reference.string()});
    QImage expected(QString::fromStdString(reference.string()));
    Check(!expected.isNull(), "Cannot load software-decoded reference");
    hm::ui::NativePreviewSurface surface;
    surface.setAttribute(Qt::WA_NativeWindow);
    surface.setAttribute(Qt::WA_PaintOnScreen);
    surface.setAttribute(Qt::WA_NoSystemBackground);
    surface.setFixedSize(640, 360);
    surface.show();
    QCoreApplication::processEvents();
    hm::ui::SourceCameraPlayer player;
    QString error;
    // Two chapters exercise both an existing decoder and chapter replacement.
    Check(
        player.Open(
            {QString::fromStdString(source.string()), QString::fromStdString(source.string())},
            surface.winId(),
            15000,
            &error),
        error.toStdString());
    const auto initial = Wait(player, [](const auto& status) { return status.presented_ns >= 0; });
    const auto chapter_ms = initial.duration_ms / 2;
    Check(target >= 0 && target < chapter_ms, "Requested frame is outside the recording");
    for (const qint64 position : {target, chapter_ms + target, target}) {
      player.Seek(position);
      Wait(player, [&](const auto& status) {
        return std::abs(status.position_ms - position) < 200 && status.presented_ns >= 0;
      });
      Check(!player.GetStatus().playing, "Paused seek resumed playback");
      Compare(Presented(surface), expected);
      Check(player.Redraw(), "Paused seek did not retain its GPU sample");
      Compare(Presented(surface), expected);
    }
    player.Play();
    Wait(player, [&](const auto& status) { return status.position_ms > target + 200; });
    player.Seek(target);
    Wait(player, [&](const auto& status) { return status.presented_ns >= 0 && status.position_ms < target + 200; });
    Check(player.GetStatus().playing, "Playing seek paused playback");
    player.Pause();
    player.Seek(target);
    Wait(player, [](const auto& status) { return status.presented_ns >= 0; });
    Compare(Presented(surface), expected);
    player.Close();
    std::cout << "Full camera pixels match software decoding after repeated and cross-chapter seeks\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
