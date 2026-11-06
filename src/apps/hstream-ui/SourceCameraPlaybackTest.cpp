#include "hstream/src/libs/common/GameOperationOwner.h"
#include "hstream/src/libs/media/TestMp4.h"
#include "src/apps/hstream-ui/HStreamWindow.h"
#include "src/apps/hstream-ui/ValueControlWheel.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
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
} // namespace
int main(int argc, char** argv) {
  try {
    hm::media::test::Directory root;
    const auto game = root.path / "game";
    for (const char* camera : {"cam1", "cam2"}) {
      std::filesystem::create_directories(game / camera);
      for (const char* chapter : {"GX010001.MP4", "GX020001.MP4"})
        hm::media::test::Ffmpeg(
            {"-f",
             "lavfi",
             "-i",
             "testsrc2=s=640x360:r=25:d=3",
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
    tabs->setCurrentIndex(0);
    Wait([&] { return play1->text() == "Play camera"; }, "Leaving camera did not pause");
    const auto paused = seek1->value();
    QTest::qWait(350);
    Check(std::abs(seek1->value() - paused) < 100, "Hidden camera kept advancing");
    tabs->setCurrentIndex(2);
    seek1->setValue(4200);
    QTest::qWait(500);
    Check(std::abs(seek1->value() - 4200) < 150, "Paused seek did not cross chapters");
    Check(play1->text() == "Play camera", "Paused seek started playback");
    Wait(
        [&] { return std::abs(target->property("sourceCameraPresentedPts").toLongLong() - 1200000000LL) < 150000000; },
        "Paused chapter seek did not present the requested source frame");
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
    Wait([&] { return seek1->value() > 4400; }, "Camera did not retain its independent position");
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
