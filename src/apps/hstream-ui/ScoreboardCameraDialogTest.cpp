#include "src/apps/hstream-ui/ScoreboardCameraDialog.h"
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include "hstream/src/libs/media/TestMp4.h"
#include "src/apps/hstream-ui/ScoreboardSelectionDialog.h"
#include "src/apps/hstream-ui/ValueControlWheel.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  install_value_control_wheel_guard();
  using hm::media::test::Check;
  try {
    Check(argc == 2, "Expected baseline");
    auto camera = YAML::LoadFile(argv[1])["rink"]["scoreboard"]["camera"];
    hm::media::test::Directory directory;
    hm::media::test::Chapter(directory.path / "camera.mp4", "red", "0");
    camera["enabled"] = true;
    camera["files"].push_back("camera.mp4");
    ScoreboardCameraDialog dialog(camera, QString::fromStdString(directory.path.string()));
    dialog.show();
    auto* preview = dialog.findChild<QPushButton*>("scoreboardCameraPreview");
    auto* save = dialog.findChild<QPushButton*>("scoreboardCameraSave");
    auto* offset = dialog.findChild<QLineEdit*>("scoreboardCameraOffset");
    auto* canvas = dynamic_cast<ScoreboardSelectionCanvas*>(dialog.findChild<QWidget*>("scoreboardSelectionCanvas"));
    Check(preview && save && offset && canvas, "Missing shared scoreboard controls");
    preview->click();
    QElapsedTimer timer;
    timer.start();
    while (!save->isEnabled() && timer.elapsed() < 6000) {
      QApplication::processEvents();
      QThread::msleep(5);
    }
    Check(
        save->isEnabled() && canvas->imageSize() == QSize(384, 192), "Preview did not retain original source geometry");
    canvas->setPoints({{96, 40}, {223, 40}, {223, 103}, {96, 103}});
    offset->setText("-00:00:05.0000");
    const auto artifacts = qEnvironmentVariable("HSTREAM_SCOREBOARD_ARTIFACTS");
    if (!artifacts.isEmpty()) {
      QDir().mkpath(artifacts);
      QApplication::processEvents();
      Check(dialog.grab().save(artifacts + "/qt-scoreboard-camera.png"), "Cannot capture scoreboard camera dialog");
    }
    auto* scroll = dialog.findChild<QScrollArea*>("scoreboardCameraControls");
    scroll->setFixedHeight(120);
    QApplication::processEvents();
    scroll->ensureWidgetVisible(offset);
    auto* bar = scroll->verticalScrollBar();
    Check(bar->value() > bar->minimum(), "Camera controls did not provide a scrollable pane");
    const int before = bar->value();
    const QPointF local(offset->rect().center());
    QWheelEvent wheel(
        local,
        offset->mapToGlobal(local.toPoint()),
        QPoint(),
        QPoint(0, 120),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false);
    QApplication::sendEvent(offset, &wheel);
    Check(
        offset->text() == "-00:00:05.0000" && bar->value() < before,
        "Wheel over offset changed its value or failed to scroll the controls");
    save->click();
    Check(dialog.result() == QDialog::Accepted, "Valid scoreboard camera was not accepted");
    const auto result = dialog.selection();
    Check(
        result["polygon"][2][0].as<int>() == 223 && result["source_dimensions"][0].as<int>() == 384 &&
            result["start_offset"].as<std::string>() == "-00:00:05.0000",
        "Crop or signed offset changed on Save");
    Check(!camera["polygon"].IsSequence(), "Dialog mutated the caller's draft before Save");
    auto fractional = YAML::Clone(result);
    fractional["polygon"][0][0] = 96.25;
    ScoreboardCameraDialog retained(fractional, QString::fromStdString(directory.path.string()));
    retained.findChild<QPushButton*>("scoreboardCameraPreview")->click();
    auto* retained_save = retained.findChild<QPushButton*>("scoreboardCameraSave");
    timer.restart();
    while (!retained_save->isEnabled() && timer.elapsed() < 6000) {
      QApplication::processEvents();
      QThread::msleep(5);
    }
    retained_save->click();
    Check(
        retained.result() == QDialog::Accepted && retained.selection()["polygon"][0][0].as<double>() == 96.25,
        "Loading a preview rounded the existing crop without an edit");
    ScoreboardCameraDialog invalid(result, QString::fromStdString(directory.path.string()));
    invalid.findChild<QLineEdit*>("scoreboardCameraOffset")->setText("00:99:00.0000");
    invalid.findChild<QPushButton*>("scoreboardCameraSave")->click();
    Check(invalid.result() != QDialog::Accepted, "Invalid offset was saved");
    invalid.findChild<QCheckBox*>("scoreboardCameraEnabled")->setChecked(false);
    invalid.findChild<QPushButton*>("scoreboardCameraSave")->click();
    Check(
        invalid.result() == QDialog::Accepted && !invalid.selection()["enabled"].as<bool>(),
        "Could not disable an invalid camera configuration");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
