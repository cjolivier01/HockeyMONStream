#include "hstream/src/libs/common/GameOperationOwner.h"
#include "hstream/src/libs/media/TestMp4.h"
#include "src/apps/hstream-ui/HStreamWindow.h"
#include "src/apps/hstream-ui/ValueControlWheel.h"

#include <QtGui/qguiapplication_platform.h>
#include <xcb/xcb.h>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtTest/QTest>
#include <QtTest/qtest_widgets.h>
#include <QtWidgets/QApplication>
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace {
using hm::media::test::Check;
template <class Predicate>
void Wait(Predicate predicate, const char* message) {
  QElapsedTimer time;
  time.start();
  while (!predicate() && time.elapsed() < 15000) {
    QCoreApplication::processEvents();
    QThread::msleep(10);
  }
  Check(predicate(), message);
}
template <class T>
T* Child(HStreamWindow& window, const char* name) {
  auto* child = window.findChild<T*>(name);
  Check(child, std::string("Missing ") + name);
  return child;
}

class RemapCounter : public QObject {
 public:
  int shows{0};
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() == QEvent::Show)
      ++shows;
    return QObject::eventFilter(watched, event);
  }
};

// Test-only bounded X11 readback checks actual front-buffer pixels. Renderer
// PTS alone cannot detect a later widget remap clearing the displayed picture.
QByteArray PresentedPixels(QWidget* target) {
  auto* connection = qGuiApp->nativeInterface<QNativeInterface::QX11Application>()->connection();
  auto* reply = xcb_get_image_reply(
      connection,
      xcb_get_image(
          connection,
          XCB_IMAGE_FORMAT_Z_PIXMAP,
          target->winId(),
          target->width() / 4,
          target->height() / 4,
          16,
          16,
          ~0U),
      nullptr);
  Check(reply, "Cannot inspect camera native pixels");
  QByteArray pixels(reinterpret_cast<const char*>(xcb_get_image_data(reply)), xcb_get_image_data_length(reply));
  std::free(reply);
  return pixels;
}
bool HasPicture(const QByteArray& pixels) {
  for (qsizetype i = 0; i + 3 < pixels.size(); i += 4)
    if (static_cast<unsigned char>(pixels[i]) > 24 || static_cast<unsigned char>(pixels[i + 1]) > 24 ||
        static_cast<unsigned char>(pixels[i + 2]) > 24)
      return true;
  return false;
}
} // namespace
int main(int argc, char** argv) {
  try {
    hm::media::test::Directory root;
    const auto game = root.path / "game";
    for (const char* camera : {"cam1", "cam2"}) {
      std::filesystem::create_directories(game / camera);
      for (const char* chapter : {"GX010001.MP4", "GX020001.MP4", "GX030001.MP4"})
        hm::media::test::Ffmpeg(
            {"-f",
             "lavfi",
             "-i",
             std::string("testsrc2=s=640x360:r=25:d=") +
                 (chapter[3] == '1'       ? "13"
                      : chapter[3] == '3' ? "1"
                                          : "3"),
             "-f",
             "lavfi",
             "-i",
             "anullsrc=r=48000:cl=stereo:d=3",
             "-c:a",
             "aac",
             "-c:v",
             "libx264",
             "-g",
             "25",
             (game / camera / chapter).string()});
    }
    std::filesystem::create_directories(game / "_hockeymon");
    // User import order differs from Program's recognized chapter order.
    std::ofstream(game / "_hockeymon/config.yaml")
        << "hstream_ui:\n  video_roles:\n"
           "    left: [cam1/GX010001.MP4, cam1/GX030001.MP4, cam1/GX020001.MP4]\n"
           "    right: [cam2/GX010001.MP4, cam2/GX030001.MP4, cam2/GX020001.MP4]\n";
    qputenv("HM_GAME_DIR", root.path.c_str());
    qputenv("HM_OUTPUT_WORK_DIR", (root.path / "output").c_str());
    qputenv("XDG_CONFIG_HOME", (root.path / "config").c_str());
    QApplication app(argc, argv);
    install_value_control_wheel_guard();
    HStreamWindow window;
    window.resize(1400, 900);
    window.show();
    window.openGame("game", false, false);
    auto* tabs = Child<QTabWidget>(window, "previewTabs");
    auto* play1 = Child<QPushButton>(window, "camera1PlayButton");
    auto* play2 = Child<QPushButton>(window, "camera2PlayButton");
    auto* seek1 = Child<QSlider>(window, "camera1SeekSlider");
    auto* stop2 = Child<QPushButton>(window, "camera2StopButton");
    Wait([&] { return play1->isEnabled(); }, "Camera playback never became available");
    tabs->setCurrentIndex(2);
    play1->click();
    Wait([&] { return seek1->maximum() > 5000 && seek1->value() > 250; }, "First camera did not play");
    const auto owner = hm::game_operation::Inspect(game);
    Check(owner.ok() && owner->has_value(), "Camera decoder did not hold recording ownership");
    const auto first_token = (**owner).token;
    auto* target = Child<QWidget>(window, "camera1PreviewRenderTarget");
    Wait(
        [&] { return target->property("sourceCameraPresentedPts").toLongLong() > 0; },
        "Hardware renderer did not present a camera frame");
    QTest::qWait(150);
    RemapCounter remaps;
    target->installEventFilter(&remaps);
    QTest::qWait(600);
    Check(remaps.shows == 0, "Camera polling remapped the video window during playback");
    play1->click();
    Wait([&] { return play1->text() == "Play camera"; }, "Camera did not pause");
    QTest::qWait(150);
    const QByteArray paused_pixels = PresentedPixels(target);
    Check(HasPicture(paused_pixels), "Paused camera lost its displayed picture");
    QTest::qWait(600);
    Check(PresentedPixels(target) == paused_pixels, "Paused polling changed the last displayed frame");
    Check(remaps.shows == 0, "Paused camera polling remapped its video window");
    play1->click();
    tabs->setCurrentIndex(0);
    Wait([&] { return play1->text() == "Play camera"; }, "Leaving camera did not pause");
    const auto paused = seek1->value();
    QTest::qWait(350);
    Check(std::abs(seek1->value() - paused) < 100, "Hidden camera kept advancing");
    tabs->setCurrentIndex(2);
    Wait([&] { return HasPicture(PresentedPixels(target)); }, "Returning to a paused camera lost its last frame");
    seek1->setValue(14200);
    QTest::qWait(500);
    Check(std::abs(seek1->value() - 14200) < 150, "Paused seek did not cross chapters");
    Check(play1->text() == "Play camera", "Paused seek started playback");
    Wait(
        [&] { return std::abs(target->property("sourceCameraPresentedPts").toLongLong() - 1200000000LL) < 150000000; },
        "Paused chapter seek did not present the requested source frame");
    target->setFocus();
    window.activateWindow();
    Check(QTest::qWaitForWindowActive(&window), "Camera window did not become active for keyboard input");
    Check(target->hasFocus(), "Camera preview did not receive keyboard focus");
    QTest::keyClick(target, Qt::Key_Left);
    Wait([&] { return std::abs(seek1->value() - 4200) < 150; }, "Left arrow did not seek backward 10 seconds");
    Check(play1->text() == "Play camera", "Left arrow resumed a paused camera");
    seek1->setFocus();
    QTest::keyClick(seek1, Qt::Key_Right);
    Wait(
        [&] { return std::abs(seek1->value() - 14200) < 150; },
        "Right arrow on slider did not seek forward 10 seconds");
    Check(play1->text() == "Play camera", "Right arrow resumed a paused camera");
    tabs->setCurrentIndex(3);
    play2->click();
    auto* seek2 = Child<QSlider>(window, "camera2SeekSlider");
    Wait([&] { return seek2->value() > 250; }, "Second camera did not play");
    Check(!Child<QPushButton>(window, "camera1StopButton")->isEnabled(), "Previous decoder remained owned");
    const auto second_owner = hm::game_operation::Inspect(game);
    Check(
        second_owner.ok() && second_owner->has_value() && (**second_owner).token != first_token,
        "Camera switch did not retire the old recording owner");
    seek2->setValue(seek2->maximum() - 80);
    Wait([&] { return play2->text() == "Play camera"; }, "Camera did not pause at end of recording");
    play2->click();
    Wait([&] { return seek2->value() > 0 && seek2->value() < 1000; }, "Play at EOS did not restart the camera");
    stop2->click();
    const auto stopped = hm::game_operation::Inspect(game);
    Check(stopped.ok() && !stopped->has_value(), "Stop retained camera recording ownership");
    tabs->setCurrentIndex(2);
    play1->click();
    Wait([&] { return seek1->value() > 14400; }, "Camera did not retain its independent position");
    target->setFocus();
    window.activateWindow();
    Check(QTest::qWaitForWindowActive(&window), "Camera window did not reactivate for the playing seek");
    Check(target->hasFocus(), "Camera preview did not regain focus for the playing seek");
    const auto before_arrow = seek1->value();
    QTest::keyClick(target, Qt::Key_Left);
    Wait(
        [&] { return std::abs(seek1->value() - (before_arrow - 10000)) < 400; },
        "Playing arrow seek did not move backward 10 seconds");
    Check(play1->text() == "Pause camera", "Arrow seek paused a running camera");
    Check(!stop2->isEnabled(), "Second decoder remained owned");
    // Another graph-building workflow retires the camera before admission.
    Child<QPushButton>(window, "startPipelineButton")->click();
    Check(!Child<QPushButton>(window, "camera1StopButton")->isEnabled(), "Program retained camera pipeline");
    std::cout << "Camera playback, chapter seek, tab pause and decoder handoff passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
