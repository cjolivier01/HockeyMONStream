#include "src/apps/hstream-ui/CameraExperimentDialog.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QTabWidget>

#include <functional>
#include <iostream>

namespace {

template <typename T>
T* widget(CameraExperimentDialog& dialog, const char* name) {
  auto* result = dialog.findChild<T*>(name);
  if (!result)
    throw std::runtime_error(std::string("Missing experiment widget: ") + name);
  return result;
}

bool wait_until(const std::function<bool()>& ready, int timeout_ms = 60000) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < timeout_ms) {
    QCoreApplication::processEvents();
    if (ready())
      return true;
    QThread::msleep(20);
  }
  return false;
}

bool smoke() {
  CameraExperimentDialog first;
  CameraExperimentDialog second;
  first.show();
  QCoreApplication::processEvents();
  auto* first_override = widget<QCheckBox>(first, "experimentOverride_Max_Speed_X_x10");
  auto* first_value = widget<QDoubleSpinBox>(first, "experimentValue_Max_Speed_X_x10");
  if (first_value->isEnabled() || widget<QPushButton>(first, "experimentApply")->isEnabled())
    return false;
  first_override->setChecked(true);
  first_value->setValue(3.5);
  if (!first_value->isEnabled() || widget<QCheckBox>(second, "experimentOverride_Max_Speed_X_x10")->isChecked())
    return false;
  widget<QLineEdit>(first, "experimentManifest")->setText("/missing/hstream_telemetry.json");
  widget<QPushButton>(first, "experimentPrepare")->click();
  if (!wait_until([&]() { return widget<QPushButton>(first, "experimentPrepare")->isEnabled(); }, 5000))
    return false;
  if (widget<QPushButton>(first, "experimentApply")->isEnabled() ||
      widget<QLabel>(first, "experimentStatus")->text().contains("Historical start ready"))
    return false;
  return true;
}

bool e2e(const QStringList& args) {
  if (args.size() < 8) {
    std::cerr << "Usage: --e2e MANIFEST PANORAMA ARTIFACT_DIR IN DURATION VIDEO_FIRST [ARENA] [CONFIG]\n";
    return false;
  }
  const QString artifacts = args[4];
  if (!QDir().mkpath(artifacts))
    return false;
  CameraExperimentDialog dialog(QFileInfo(args[2]).absolutePath());
  dialog.show();
  widget<QLineEdit>(dialog, "experimentManifest")->setText(args[2]);
  widget<QLineEdit>(dialog, "experimentMedia")->setText(args[3]);
  widget<QDoubleSpinBox>(dialog, "experimentIn")->setValue(args[5].toDouble());
  widget<QDoubleSpinBox>(dialog, "experimentDuration")->setValue(args[6].toDouble());
  widget<QDoubleSpinBox>(dialog, "experimentVideoOrigin")->setValue(args[7].toDouble());
  widget<QCheckBox>(dialog, "experimentUncropped")->setChecked(true);
  if (args.size() > 8)
    widget<QLineEdit>(dialog, "experimentLegacyArena")->setText(args[8]);
  if (args.size() > 9)
    widget<QLineEdit>(dialog, "experimentLegacyConfig")->setText(args[9]);
  auto* prepare = widget<QPushButton>(dialog, "experimentPrepare");
  auto* apply = widget<QPushButton>(dialog, "experimentApply");
  auto* status = widget<QLabel>(dialog, "experimentStatus");
  prepare->click();
  if (!wait_until([&]() { return prepare->isEnabled(); }) || !apply->isEnabled()) {
    std::cerr << "Prepare failed: " << status->text().toStdString() << '\n';
    return false;
  }
  widget<QCheckBox>(dialog, "experimentOverride_Max_Speed_X_x10")->setChecked(true);
  widget<QDoubleSpinBox>(dialog, "experimentValue_Max_Speed_X_x10")->setValue(1.5);
  widget<QCheckBox>(dialog, "experimentOverride_Max_Accel_X_x10")->setChecked(true);
  widget<QDoubleSpinBox>(dialog, "experimentValue_Max_Accel_X_x10")->setValue(0.2);
  widget<QLineEdit>(dialog, "experimentTrialName")->setText("A · slower pan");
  apply->click();
  auto* comparison = widget<QComboBox>(dialog, "experimentComparison");
  auto* screenshot = widget<QPushButton>(dialog, "experimentScreenshot");
  if (!wait_until([&]() {
        return (comparison->count() == 3 && screenshot->isEnabled()) || status->styleSheet().contains("#b42318");
      }) ||
      !screenshot->isEnabled()) {
    std::cerr << "Trial preview failed: " << status->text().toStdString() << '\n';
    return false;
  }
  const auto capture = [&](const char* name) {
    QString error;
    if (!dialog.captureScreenshot(QDir(artifacts).filePath(name), &error)) {
      std::cerr << error.toStdString() << '\n';
      return false;
    }
    return true;
  };
  // Require visible frame progress before using screenshots as playback evidence.
  auto* play = widget<QPushButton>(dialog, "experimentPlay");
  const QString first_frame = widget<QLabel>(dialog, "experimentFrameStatus")->text();
  if (!wait_until(
          [&]() {
            return widget<QLabel>(dialog, "experimentFrameStatus")->text() != first_frame && screenshot->isEnabled();
          },
          10000)) {
    std::cerr << "Video made no presented-frame progress: " << status->text().toStdString() << '\n';
    return false;
  }
  if (play->text() == "Pause")
    play->click();
  widget<QTabWidget>(dialog, "experimentControlTabs")->setCurrentIndex(1);
  auto* timeline = widget<QSlider>(dialog, "experimentTimeline");
  const int comparison_frame = timeline->maximum() / 2;
  auto seek_frame = [&](int frame) {
    timeline->setValue(frame);
    QMetaObject::invokeMethod(timeline, "sliderReleased", Qt::DirectConnection);
    return wait_until([&]() { return screenshot->isEnabled() || status->styleSheet().contains("#b42318"); }, 20000) &&
        screenshot->isEnabled() && timeline->value() == frame;
  };
  if (!seek_frame(comparison_frame)) {
    std::cerr << "Midrange seek failed: " << status->text().toStdString() << '\n';
    return false;
  }
  if (!capture("experiment-trial-a.png"))
    return false;
  const QString before_step = widget<QLabel>(dialog, "experimentFrameStatus")->text();
  widget<QPushButton>(dialog, "experimentNext")->click();
  if (!wait_until(
          [&]() {
            return screenshot->isEnabled() && widget<QLabel>(dialog, "experimentFrameStatus")->text() != before_step;
          },
          20000)) {
    std::cerr << "Paused frame step failed: " << status->text().toStdString() << '\n';
    return false;
  }
  comparison->setCurrentIndex(0);
  if (!seek_frame(comparison_frame) || !capture("experiment-original.png"))
    return false;
  // Exercise the actual end boundary without waiting through the entire clip.
  // A successful loop must present a new frame near its beginning.
  comparison->setCurrentIndex(2);
  if (!seek_frame(timeline->maximum() - 6))
    return false;
  play->click();
  if (!wait_until(
          [&]() {
            return (timeline->value() < 20 && screenshot->isEnabled()) || status->styleSheet().contains("#b42318");
          },
          20000) ||
      !screenshot->isEnabled() || timeline->value() >= 20) {
    std::cerr << "Range loop failed: " << status->text().toStdString() << '\n';
    return false;
  }
  play->click();
  widget<QCheckBox>(dialog, "experimentLoop")->setChecked(false);
  if (!seek_frame(timeline->maximum() - 6))
    return false;
  play->click();
  if (!wait_until(
          [&]() {
            return (timeline->value() == timeline->maximum() && play->text() != "Pause" && screenshot->isEnabled()) ||
                status->styleSheet().contains("#b42318");
          },
          20000) ||
      timeline->value() != timeline->maximum() || !screenshot->isEnabled()) {
    std::cerr << "End-of-range pause failed: " << status->text().toStdString() << '\n';
    return false;
  }
  std::cout
      << "PASS: historical preparation, parameter trial, actual GPU playback, paused frame step, original comparison, range loop, end pause, screenshots\n";
  return true;
}

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    if (app.arguments().contains("--e2e"))
      return e2e(app.arguments()) ? 0 : 1;
    return smoke() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
