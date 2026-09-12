#include "src/apps/hstream-ui/CameraExperimentDialog.h"
#include "src/apps/hstream-ui/CameraExperimentPreviewWorker.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QTabWidget>

#include <atomic>
#include <functional>
#include <future>
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
  if (!widget<QProgressBar>(first, "experimentProgress")->isVisible() ||
      widget<QTabWidget>(first, "experimentControlTabs")->isEnabled() ||
      widget<QLineEdit>(first, "experimentManifest")->isEnabled() ||
      !widget<QPushButton>(first, "experimentCancel")->isEnabled())
    return false;
  if (!wait_until([&]() { return widget<QPushButton>(first, "experimentPrepare")->isEnabled(); }, 5000))
    return false;
  if (widget<QPushButton>(first, "experimentApply")->isEnabled() ||
      widget<QLabel>(first, "experimentStatus")->text().contains("Historical start ready"))
    return false;
  CameraExperimentDialog seeded(
      {},
      nullptr,
      {{"Ignore_Largest_Count", 1},
       {"Max_Speed_X_x10", 35},
       {"Oversized_Player_Percent", 1234.567890123},
       {"Apply_To_Fast_Box", 1},
       {"Apply_To_Follower_Box", 0}});
  if (widget<QDoubleSpinBox>(seeded, "experimentValue_Ignore_Largest_Count")->value() != 1 ||
      !widget<QCheckBox>(seeded, "experimentOverride_Ignore_Largest_Count")->isChecked() ||
      widget<QDoubleSpinBox>(seeded, "experimentValue_Max_Speed_X_x10")->value() != 3.5 ||
      widget<QDoubleSpinBox>(seeded, "experimentValue_Oversized_Player_Percent")->value() != 1234.567890123 ||
      !widget<QCheckBox>(seeded, "experimentFastBox")->isChecked() ||
      widget<QCheckBox>(seeded, "experimentFollowerBox")->isChecked())
    return false;
  CameraExperimentDialog small_percentage({}, nullptr, {{"Oversized_Player_Percent", 0.004}});
  if (widget<QDoubleSpinBox>(small_percentage, "experimentValue_Oversized_Player_Percent")->value() != 0.004)
    return false;
  widget<QDoubleSpinBox>(seeded, "experimentValue_Ignore_Largest_Count")->setValue(3);
  if (widget<QDoubleSpinBox>(second, "experimentValue_Ignore_Largest_Count")->value() != 0)
    return false;
  second.show();
  widget<QLineEdit>(second, "experimentManifest")->setText("/missing/hstream_telemetry.json");
  widget<QPushButton>(second, "experimentPrepare")->click();
  second.reject();
  if (!second.isVisible() || !wait_until([&] { return !second.isVisible(); }, 5000))
    return false;
  return true;
}

bool responsive_worker() {
  CameraExperimentPreviewWorker worker;
  std::promise<void> entered, release;
  auto started = entered.get_future();
  const auto released = release.get_future().share();
  worker.Submit([&entered, released](auto&, auto* error) {
    entered.set_value();
    released.wait_for(std::chrono::seconds(5));
    *error = "stale operation failure";
    return false;
  });
  if (!wait_until([&] { return started.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }, 1000)) {
    release.set_value();
    return false;
  }
  int heartbeat = 0;
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, [&] { ++heartbeat; });
  timer.start(10);
  if (!wait_until([&] { return heartbeat >= 5; }, 500) || !worker.Poll().busy) {
    release.set_value();
    return false;
  }
  std::atomic<bool> obsolete_ran{false};
  worker.Submit([&](auto&, auto*) {
    obsolete_ran = true;
    return true;
  });
  QElapsedTimer elapsed;
  elapsed.start();
  worker.Close();
  const bool responsive = elapsed.elapsed() < 500 && worker.Poll().busy;
  release.set_value();
  if (!responsive || !wait_until([&] { return !worker.Poll().busy; }, 2000) || obsolete_ran ||
      !worker.Poll().error.empty() || worker.Poll().open)
    return false;
  worker.Submit([](auto&, auto* error) {
    *error = "visible failure";
    return false;
  });
  if (!wait_until([&] { return !worker.Poll().busy; }, 2000) || worker.Poll().error != "visible failure")
    return false;
  if (wait_until([&] { return worker.Poll().error != "visible failure"; }, 150))
    return false;
  return true;
}

bool require_confirmation(CameraExperimentDialog& dialog) {
  auto* confirmed = widget<QCheckBox>(dialog, "experimentUncropped");
  auto* apply = widget<QPushButton>(dialog, "experimentApply");
  auto* prepare = widget<QPushButton>(dialog, "experimentPrepare");
  auto* comparison = widget<QComboBox>(dialog, "experimentComparison");
  auto* name = widget<QLineEdit>(dialog, "experimentTrialName");
  auto* status = widget<QLabel>(dialog, "experimentStatus");
  const int count = comparison->count();
  const QString trial_name = name->text();
  confirmed->setChecked(false);
  if (!wait_until([&] { return apply->isEnabled(); }, 2000))
    return false;
  for (int attempt = 0; attempt < 2; ++attempt) {
    apply->click();
    if (!prepare->isEnabled() || !apply->isEnabled() || comparison->count() != count || name->text() != trial_name ||
        !status->text().contains(confirmed->text()) || !status->styleSheet().contains("#b42318")) {
      std::cerr << "Unconfirmed replay started a trial or failed to explain the required checkbox\n";
      return false;
    }
  }
  const QString message = status->text();
  // Let several worker/preview polls run: the actionable message must persist.
  if (wait_until([&]() { return status->text() != message || comparison->count() != count; }, 150)) {
    std::cerr << "Source confirmation message was overwritten or a blocked trial was added\n";
    return false;
  }
  confirmed->setChecked(true);
  return true;
}

bool confirmation(const QString& manifest) {
  CameraExperimentDialog dialog;
  dialog.show();
  widget<QLineEdit>(dialog, "experimentManifest")->setText(manifest);
  widget<QDoubleSpinBox>(dialog, "experimentDuration")->setValue(1);
  auto* prepare = widget<QPushButton>(dialog, "experimentPrepare");
  auto* apply = widget<QPushButton>(dialog, "experimentApply");
  prepare->click();
  if (!wait_until([&]() { return prepare->isEnabled(); }) || !apply->isEnabled() || !require_confirmation(dialog))
    return false;
  apply->click();
  if (!wait_until([&]() { return prepare->isEnabled(); }) ||
      widget<QComboBox>(dialog, "experimentComparison")->count() != 3 ||
      widget<QLineEdit>(dialog, "experimentTrialName")->text() != "Trial 2") {
    std::cerr << "Confirming the source did not allow the trial to be calculated\n";
    return false;
  }
  std::cout << "PASS: unconfirmed replay creates no trial, message persists, confirmation allows a trial\n";
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
  QElapsedTimer heartbeat;
  heartbeat.start();
  qint64 longest_gap = 0;
  QTimer responsiveness;
  QObject::connect(
      &responsiveness, &QTimer::timeout, [&] { longest_gap = std::max(longest_gap, heartbeat.restart()); });
  responsiveness.start(10);
  CameraExperimentDialog dialog(QFileInfo(args[2]).absolutePath());
  dialog.show();
  widget<QLineEdit>(dialog, "experimentManifest")->setText(args[2]);
  widget<QLineEdit>(dialog, "experimentMedia")->setText(args[3]);
  if (QFileInfo(args[3]).isDir()) {
    widget<QComboBox>(dialog, "experimentSourceMode")->setCurrentIndex(0);
    widget<QLineEdit>(dialog, "experimentGameDirectory")->setText(args[3]);
  } else {
    widget<QComboBox>(dialog, "experimentSourceMode")->setCurrentIndex(1);
  }
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
  if (!require_confirmation(dialog))
    return false;
  widget<QCheckBox>(dialog, "experimentOverride_Max_Speed_X_x10")->setChecked(true);
  widget<QDoubleSpinBox>(dialog, "experimentValue_Max_Speed_X_x10")->setValue(1.5);
  widget<QCheckBox>(dialog, "experimentOverride_Max_Accel_X_x10")->setChecked(true);
  widget<QDoubleSpinBox>(dialog, "experimentValue_Max_Accel_X_x10")->setValue(0.2);
  widget<QLineEdit>(dialog, "experimentTrialName")->setText("A · slower pan");
  apply->click();
  auto* comparison = widget<QComboBox>(dialog, "experimentComparison");
  auto* screenshot = widget<QPushButton>(dialog, "experimentScreenshot");
  if (!widget<QProgressBar>(dialog, "experimentProgress")->isVisible() ||
      widget<QTabWidget>(dialog, "experimentControlTabs")->isEnabled() ||
      widget<QLineEdit>(dialog, "experimentManifest")->isEnabled())
    return false;
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
  if (!wait_until([&] { return widget<QTabWidget>(dialog, "experimentControlTabs")->isEnabled(); }, 60000)) {
    std::cerr << "Pause did not restore editable camera controls\n";
    return false;
  }
  widget<QTabWidget>(dialog, "experimentControlTabs")->setCurrentIndex(1);
  auto* timeline = widget<QSlider>(dialog, "experimentTimeline");
  const int comparison_frame = timeline->maximum() / 2;
  auto seek_frame = [&](int frame) {
    timeline->setValue(frame);
    QMetaObject::invokeMethod(timeline, "sliderReleased", Qt::DirectConnection);
    return wait_until([&]() { return screenshot->isEnabled() || status->styleSheet().contains("#b42318"); }, 60000) &&
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
          60000)) {
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
          60000) ||
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
          60000) ||
      timeline->value() != timeline->maximum() || !screenshot->isEnabled()) {
    std::cerr << "End-of-range pause failed: " << status->text().toStdString() << '\n';
    return false;
  }
  auto* save = widget<QPushButton>(dialog, "experimentSave");
  if (!save->isEnabled()) {
    std::cerr << "Prepared candidate cannot be saved\n";
    return false;
  }
  auto* in = widget<QDoubleSpinBox>(dialog, "experimentIn");
  in->setValue(in->value() + 1);
  if (save->isEnabled()) {
    std::cerr << "Changing the range left an old candidate saveable with a different media binding\n";
    return false;
  }
  comparison->setCurrentIndex(0);
  comparison->setCurrentIndex(2);
  if (save->isEnabled()) {
    std::cerr << "Selecting an old candidate enabled Save before the new range was prepared\n";
    return false;
  }
  // A failed preparation retains the old candidate for inspection. It must
  // not make that candidate saveable using the edited recording or range.
  widget<QLineEdit>(dialog, "experimentManifest")->setText("/missing/hstream_telemetry.json");
  prepare->click();
  if (!wait_until([&]() { return prepare->isEnabled(); }, 5000) || save->isEnabled()) {
    std::cerr << "Failed preparation re-enabled Save for a stale candidate\n";
    return false;
  }
  std::cout
      << "PASS: historical preparation, parameter trial, actual GPU playback, paused frame step, original comparison, range loop, end pause, screenshots, stale trial save invalidation\n";
  std::cout << "Longest UI heartbeat gap: " << longest_gap << " ms\n";
  if (longest_gap > 1000) {
    std::cerr << "Experiment blocked the UI event loop for more than one second\n";
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    if (app.arguments().contains("--e2e"))
      return e2e(app.arguments()) ? 0 : 1;
    if (app.arguments().size() == 3 && app.arguments()[1] == "--confirmation")
      return confirmation(app.arguments()[2]) ? 0 : 1;
    return smoke() && responsive_worker() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
