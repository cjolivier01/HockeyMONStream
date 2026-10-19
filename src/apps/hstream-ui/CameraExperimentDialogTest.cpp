#include "src/apps/hstream-ui/CameraExperimentDialog.h"
#include "src/apps/hstream-ui/CameraExperimentPreviewWorker.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizeGrip>
#include <QtWidgets/QSlider>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QToolButton>

#include <atomic>
#include <cmath>
#include <functional>
#include <future>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

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
  CameraExperimentDialog first({}, nullptr, {}, 420000);
  CameraExperimentDialog second;
  auto* playback_start = widget<QDoubleSpinBox>(first, "experimentPlaybackStart");
  auto* start_offset = widget<QDoubleSpinBox>(first, "experimentIn");
  if (playback_start->value() != 420 || playback_start->text() != "00:07:00") {
    std::cerr << "Camera experiment did not inherit Playback start\n";
    return false;
  }
  for (const auto& [text, seconds] :
       std::vector<std::pair<QString, double>>{{"90:23", 5423}, {"12.125", 12.125}, {"01:02:03.5", 3723.5}}) {
    start_offset->findChild<QLineEdit*>()->setText(text);
    start_offset->interpretText();
    if (start_offset->value() != seconds) {
      std::cerr << "Camera experiment rejected an abbreviated offset\n";
      return false;
    }
  }
  start_offset->findChild<QLineEdit*>()->setText("-1");
  start_offset->interpretText();
  if (start_offset->value() < 0)
    return false;
  playback_start->setValue(600);
  if (widget<QDoubleSpinBox>(second, "experimentPlaybackStart")->value() != 0) {
    std::cerr << "Camera experiment playback origin leaked to another dialog\n";
    return false;
  }
  first.show();
  QCoreApplication::processEvents();
  auto* rink_aware = widget<QCheckBox>(first, "experimentRinkAware");
  if (!rink_aware || rink_aware->isEnabled() || rink_aware->isChecked())
    return false;
  start_offset->setFocus();
  const double before_scroll = start_offset->value();
  const QPoint local = start_offset->rect().center();
  QWheelEvent wheel(
      local,
      start_offset->mapToGlobal(local),
      {},
      QPoint(0, 120),
      Qt::NoButton,
      Qt::NoModifier,
      Qt::NoScrollPhase,
      false);
  QApplication::sendEvent(start_offset, &wheel);
  if (start_offset->value() != before_scroll) {
    std::cerr << "Wheel input changed a focused experiment value\n";
    return false;
  }
  auto* maximize_window = widget<QToolButton>(first, "maximizeCameraExperimentWindowButton");
  auto* resize_grip = first.findChild<QSizeGrip*>();
  if (!first.windowFlags().testFlag(Qt::WindowMaximizeButtonHint) || !first.isSizeGripEnabled() || !resize_grip ||
      !resize_grip->isVisible()) {
    std::cerr << "Camera experiments window must support maximize and resize\n";
    return false;
  }
  const QSize resized = first.size() + QSize(80, 60);
  first.resize(resized);
  if (!wait_until([&] { return first.size() == resized; }, 2000)) {
    std::cerr << "Camera experiments window did not resize: requested " << resized.width() << 'x' << resized.height()
              << ", got " << first.width() << 'x' << first.height() << '\n';
    return false;
  }
  maximize_window->click();
  if (!wait_until([&] { return first.isMaximized() && maximize_window->text() == "Restore window"; }, 2000)) {
    std::cerr << "Camera experiments window did not maximize\n";
    return false;
  }
  maximize_window->click();
  if (!wait_until([&] { return !first.isMaximized() && maximize_window->text() == "Maximize window"; }, 2000)) {
    std::cerr << "Camera experiments window did not restore\n";
    return false;
  }
  auto* preview_focus = widget<QToolButton>(first, "maximizeCameraExperimentPreviewButton");
  auto* preview_stop = widget<QPushButton>(first, "experimentStopPreview");
  auto* realtime = widget<QCheckBox>(first, "experimentRealtimePlayback");
  if (!realtime || !realtime->isEnabled() || !realtime->isChecked()) {
    std::cerr << "Camera preview should expose its existing normal-speed default\n";
    return false;
  }
  realtime->setChecked(false);
  if (!widget<QCheckBox>(second, "experimentRealtimePlayback")->isChecked())
    return false;
  for (const char* name : {"experimentDrawPlayers", "experimentDrawPlayTracking"}) {
    widget<QCheckBox>(first, name)->setChecked(true);
    if (widget<QCheckBox>(second, name)->isChecked())
      return false;
  }
  if (widget<QCheckBox>(first, "experimentDrawRinkMask")->isEnabled())
    return false;
  // Every plot drawing is switchable before anything is prepared, starts in its
  // documented state, explains itself, and belongs to one dialog. Repaint in
  // both states too: an unprepared plot has no frames to draw, which is the
  // case every empty-run guard in paintEvent has to survive.
  auto* camera_plot = widget<QWidget>(first, "experimentCameraPlot");
  for (const auto& [name, on_by_default] : std::vector<std::pair<const char*, bool>>{
           {"experimentPlotLayer_Recorded", true},
           {"experimentPlotLayer_Program", true},
           {"experimentPlotLayer_Crop", true},
           {"experimentPlotLayer_Fast", false},
           {"experimentPlotLayer_Players", false},
           {"experimentPlotLayer_Cluster", false},
           {"experimentPlotLayer_Breakaway", true}}) {
    auto* toggle = widget<QCheckBox>(first, name);
    if (!toggle->isEnabled() || toggle->isChecked() != on_by_default || toggle->toolTip().isEmpty()) {
      std::cerr << "Camera plot layer is unusable or starts in the wrong state: " << name << '\n';
      return false;
    }
    toggle->setChecked(!on_by_default);
    if (widget<QCheckBox>(second, name)->isChecked() != on_by_default) {
      std::cerr << "Camera plot layer leaked to another dialog: " << name << '\n';
      return false;
    }
    if (camera_plot->grab().isNull()) {
      std::cerr << "Camera plot did not paint with layer flipped: " << name << '\n';
      return false;
    }
    toggle->setChecked(on_by_default);
    if (camera_plot->grab().isNull()) {
      std::cerr << "Camera plot did not paint with layer restored: " << name << '\n';
      return false;
    }
  }
  auto* controls_panel = widget<QWidget>(first, "experimentControlsPanel");
  auto* plot_panel = widget<QWidget>(first, "experimentCameraPlotPanel");
  auto* video = widget<QWidget>(first, "experimentVideoTarget");
  const WId original_target = video->winId();
  if (!preview_focus->text().isEmpty() || preview_focus->accessibleName() != "Maximize preview")
    return false;
  preview_focus->click();
  QCoreApplication::processEvents();
  if (controls_panel->isVisible() || plot_panel->isVisible() || preview_focus->accessibleName() != "Restore preview" ||
      video->winId() != original_target || !preview_stop->isEnabled()) {
    std::cerr << "Camera preview did not focus while retaining its GPU target\n";
    return false;
  }
  preview_stop->click();
  QCoreApplication::processEvents();
  if (!controls_panel->isVisible() || !plot_panel->isVisible() ||
      preview_focus->accessibleName() != "Maximize preview") {
    std::cerr << "Stopping camera preview did not restore its layout\n";
    return false;
  }
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
       {"Stop_Delay_Cooldown_Frames", 90},
       {"Oversized_Player_Percent", 1234.567890123},
       {"Apply_To_Fast_Box", 1},
       {"Apply_To_Follower_Box", 0}});
  if (widget<QDoubleSpinBox>(seeded, "experimentValue_Ignore_Largest_Count")->value() != 1 ||
      !widget<QCheckBox>(seeded, "experimentOverride_Ignore_Largest_Count")->isChecked() ||
      widget<QDoubleSpinBox>(seeded, "experimentValue_Max_Speed_X_x10")->value() != 3.5 ||
      widget<QDoubleSpinBox>(seeded, "experimentValue_Stop_Delay_Cooldown_Frames")->value() != 90 ||
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
  worker.Close();
  if (worker.Poll().busy)
    return false;
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
  for (int i = 0; i < 100; ++i)
    worker.RequestRedraw();
  if (wait_until(
          [&] {
            const auto state = worker.Poll();
            return state.error != "visible failure" || state.busy || state.open;
          },
          150))
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

bool rink_comparison(const QStringList& args) {
  if (args.size() != 8) {
    std::cerr << "Usage: --rink-comparison RECORDING.db GAME_DIRECTORY ARTIFACT_DIR OFFSET DURATION PLAYBACK_START\n";
    return false;
  }
  if (!QDir().mkpath(args[4]))
    return false;
  CameraExperimentDialog dialog(args[3], nullptr, {}, std::llround(args[7].toDouble() * 1000));
  dialog.setWindowFlag(Qt::WindowTransparentForInput);
  dialog.setAttribute(Qt::WA_ShowWithoutActivating);
  dialog.show();
  widget<QLineEdit>(dialog, "experimentManifest")->setText(args[2]);
  widget<QComboBox>(dialog, "experimentSourceMode")->setCurrentIndex(0);
  widget<QLineEdit>(dialog, "experimentGameDirectory")->setText(args[3]);
  widget<QDoubleSpinBox>(dialog, "experimentIn")->setValue(args[5].toDouble());
  widget<QDoubleSpinBox>(dialog, "experimentDuration")->setValue(args[6].toDouble());
  widget<QDoubleSpinBox>(dialog, "experimentVideoOrigin")->setValue(args[5].toDouble());
  widget<QCheckBox>(dialog, "experimentUncropped")->setChecked(true);
  widget<QCheckBox>(dialog, "experimentLoop")->setChecked(false);
  auto* prepare = widget<QPushButton>(dialog, "experimentPrepare");
  auto* apply = widget<QPushButton>(dialog, "experimentApply");
  auto* status = widget<QLabel>(dialog, "experimentStatus");
  prepare->click();
  if (!wait_until([&] { return prepare->isEnabled(); }, 60000) || !apply->isEnabled()) {
    std::cerr << "Prepare failed: " << status->text().toStdString() << '\n';
    return false;
  }
  auto* enabled = widget<QCheckBox>(dialog, "experimentRinkAware");
  if (!enabled->isEnabled()) {
    std::cerr << widget<QLabel>(dialog, "experimentRinkStatus")->text().toStdString() << '\n';
    return false;
  }
  enabled->setChecked(true);
  widget<QTabWidget>(dialog, "experimentControlTabs")->setCurrentIndex(3);
  widget<QLineEdit>(dialog, "experimentTrialName")->setText("Rink-aware framing");
  apply->click();
  auto* screenshot = widget<QPushButton>(dialog, "experimentScreenshot");
  if (!wait_until([&] { return screenshot->isEnabled() || status->styleSheet().contains("#b42318"); }, 60000) ||
      !screenshot->isEnabled()) {
    std::cerr << "Trial preview failed: " << status->text().toStdString() << '\n';
    return false;
  }
  auto* play = widget<QPushButton>(dialog, "experimentPlay");
  auto* views = widget<QComboBox>(dialog, "experimentComparison");
  auto* timeline = widget<QSlider>(dialog, "experimentTimeline");
  auto* target = widget<QWidget>(dialog, "experimentVideoTarget");
  auto* label = widget<QLabel>(dialog, "experimentFrameStatus");
  // Exercise moving playback for both trajectories before matched stills.
  if (views->count() != 4 || !views->itemText(2).contains("native") || !views->itemText(3).contains("rink-aware"))
    return false;
  for (int view : {3, 2}) {
    views->setCurrentIndex(view);
    if (!wait_until([&] { return screenshot->isEnabled(); }, 60000))
      return false;
    const int first = timeline->value();
    if (play->text() != "Pause")
      play->click();
    if (!wait_until([&] { return screenshot->isEnabled() && timeline->value() > first + 60; }, 60000)) {
      std::cerr << "Comparison playback made no progress: " << status->text().toStdString() << '\n';
      return false;
    }
    play->click();
    if (!wait_until([&] { return apply->isEnabled(); }, 60000))
      return false;
  }
  for (int part : {1, 2, 3}) {
    const int frame = timeline->maximum() * part / 4;
    for (int view : {2, 3}) {
      const int inspected = timeline->value();
      views->setCurrentIndex(view);
      if (!wait_until([&] { return screenshot->isEnabled(); }, 60000))
        return false;
      if (timeline->value() != inspected) {
        std::cerr << "Switching paused A/B views changed the inspected sample\n";
        return false;
      }
      timeline->setValue(frame);
      QMetaObject::invokeMethod(timeline, "sliderReleased", Qt::DirectConnection);
      if (!wait_until([&] { return screenshot->isEnabled() || status->styleSheet().contains("#b42318"); }, 60000) ||
          !screenshot->isEnabled() || timeline->value() != frame)
        return false;
      QString error;
      const QString name = QString("%1-%2").arg(view == 2 ? "baseline" : "rink").arg(part);
      const QString path = QDir(args[4]).filePath(name + ".png");
      if (!dialog.captureScreenshot(path, &error)) {
        std::cerr << error.toStdString() << '\n';
        return false;
      }
      const QImage screenshot_image(path);
      const double scale = double(screenshot_image.width()) / dialog.width();
      const QPoint origin = target->mapTo(&dialog, QPoint());
      const QImage image = screenshot_image.copy(QRect(
          qRound(origin.x() * scale),
          qRound(origin.y() * scale),
          qRound(target->width() * scale),
          qRound(target->height() * scale)));
      const QSize fitted = QSize(1280, 720).scaled(image.size(), Qt::KeepAspectRatio);
      const QRect video_rect(
          (image.width() - fitted.width()) / 2,
          (image.height() - fitted.height()) / 2,
          fitted.width(),
          fitted.height());
      if (!image.copy(video_rect).save(QDir(args[4]).filePath(name + "-video.png")))
        return false;
      std::cout << name.toStdString() << ": " << label->text().toStdString() << '\n';
    }
  }
  widget<QPushButton>(dialog, "experimentStopPreview")->click();
  std::cout << "PASS: rink-aware trial, moving A/B playback, exact-sample native GPU comparisons\n";
  return true;
}

bool e2e(const QStringList& args) {
  if (args.size() < 8) {
    std::cerr
        << "Usage: --e2e MANIFEST PANORAMA ARTIFACT_DIR OFFSET DURATION VIDEO_OFFSET [ARENA] [CONFIG] [PLAYBACK_START]\n";
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
  CameraExperimentDialog dialog(
      QFileInfo(args[2]).absolutePath(), nullptr, {}, args.size() > 10 ? std::llround(args[10].toDouble() * 1000) : 0);
  // The GPU test shares the desktop; only its synthetic clicks should seek.
  dialog.setWindowFlag(Qt::WindowTransparentForInput);
  dialog.setAttribute(Qt::WA_ShowWithoutActivating);
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
  auto* realtime = widget<QCheckBox>(dialog, "experimentRealtimePlayback");
  if (play->text() == "Pause" && realtime->isEnabled()) {
    std::cerr << "Playback pacing must be locked while the preview is playing\n";
    return false;
  }
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
  if (!realtime->isEnabled())
    return false;
  widget<QTabWidget>(dialog, "experimentControlTabs")->setCurrentIndex(1);
  auto* timeline = widget<QSlider>(dialog, "experimentTimeline");
  auto* target = widget<QWidget>(dialog, "experimentVideoTarget");
  auto* plot = widget<QWidget>(dialog, "experimentCameraPlot");
  // Inspect the GPU front buffer: QWidget::grab omits native video, and X11
  // screen grabs can be black when this display is locked or composited.
  const auto visible_preview = [&](const char* action) {
    QElapsedTimer settle;
    settle.start();
    if (wait_until(
            [&] {
              if (settle.elapsed() < 200 || !screenshot->isEnabled())
                return false;
              if (!capture("paused-preview.png"))
                return false;
              const QImage screenshot_image(QDir(artifacts).filePath("paused-preview.png"));
              const double scale = double(screenshot_image.width()) / dialog.width();
              const QPoint origin = target->mapTo(&dialog, QPoint(0, 0));
              const QImage image = screenshot_image.copy(QRect(
                  qRound(origin.x() * scale), qRound(origin.y() * scale),
                  qRound(target->width() * scale), qRound(target->height() * scale)));
              int bright = 0, samples = 0;
              for (int y = 0; y < image.height(); y += 8)
                for (int x = 0; x < image.width(); x += 8) {
                  bright += qGray(image.pixel(x, y)) > 32;
                  ++samples;
                }
              return samples > 0 && bright > samples / 10;
            },
            5000))
      return true;
    std::cerr << "Native preview lost its image after " << action << '\n';
    capture("failed-dialog.png");
    return false;
  };
  if (!visible_preview("Pause"))
    return false;
  const int paused_frame = timeline->value();
  const QSize normal_size = dialog.size();
  for (const QSize size : {normal_size + QSize(100, 80), normal_size}) {
    dialog.resize(size);
    if (!visible_preview("paused resize") || timeline->value() != paused_frame)
      return false;
  }
  auto* focus = widget<QToolButton>(dialog, "maximizeCameraExperimentPreviewButton");
  for (int i = 0; i < 2; ++i) {
    focus->click();
    if (!visible_preview("paused preview maximize/restore") || timeline->value() != paused_frame)
      return false;
  }
  auto* maximize = widget<QToolButton>(dialog, "maximizeCameraExperimentWindowButton");
  for (int i = 0; i < 2; ++i) {
    maximize->click();
    if (!visible_preview("paused window maximize/restore") || timeline->value() != paused_frame)
      return false;
  }
  const auto click_plot_frame = [&](int frame) {
    const QPointF position(14.0 + (plot->width() - 28.0) * frame / timeline->maximum(), plot->height() / 2.0);
    QMouseEvent press(
        QEvent::MouseButtonPress,
        position,
        plot->mapToGlobal(position),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    QApplication::sendEvent(plot, &press);
    QMouseEvent release(
        QEvent::MouseButtonRelease,
        position,
        plot->mapToGlobal(position),
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier);
    QApplication::sendEvent(plot, &release);
    if (!wait_until([&] { return screenshot->isEnabled() || status->styleSheet().contains("#b42318"); }) ||
        !screenshot->isEnabled() || timeline->value() != frame || play->text() == "Pause") {
      std::cerr << "Visual timeline did not seek and pause at frame " << frame << ": " << status->text().toStdString()
                << '\n';
      return false;
    }
    return visible_preview("visual timeline seek");
  };
  if (plot->accessibleName() != "Visual timeline")
    return false;
  const int comparison_frame = timeline->maximum() / 2;
  for (const int frame : {0, timeline->maximum(), comparison_frame})
    if (!click_plot_frame(frame))
      return false;
  play->click();
  if (!wait_until([&] { return screenshot->isEnabled() && timeline->value() > comparison_frame; }, 60000)) {
    std::cerr << "Play did not resume from the visual timeline selection\n";
    return false;
  }
  if (!click_plot_frame(comparison_frame)) // Clicking during playback also pauses on the requested frame.
    return false;
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
  // Change one layer at a time on the same paused frame. Compare only GPU
  // preview pixels, so checkbox painting cannot satisfy the drawing check.
  const QRect logical_rect(target->mapTo(&dialog, QPoint(0, 0)), target->size());
  const QImage clean_capture(QDir(artifacts).filePath("experiment-trial-a.png"));
  const double sx = double(clean_capture.width()) / dialog.width();
  const double sy = double(clean_capture.height()) / dialog.height();
  const QRect video_rect(
      qRound(logical_rect.x() * sx),
      qRound(logical_rect.y() * sy),
      qRound(logical_rect.width() * sx),
      qRound(logical_rect.height() * sy));
  const QImage clean = clean_capture.copy(video_rect);
  for (const char* name : {"experimentDrawPlayers", "experimentDrawPlayTracking", "experimentDrawRinkMask"}) {
    auto* toggle = widget<QCheckBox>(dialog, name);
    if (!toggle->isEnabled()) {
      if (std::string(name) == "experimentDrawRinkMask" && !args[2].endsWith(".db"))
        continue; // Legacy CSV contains no historical mask.
      std::cerr << "Missing prepared drawing: " << name << '\n';
      return false;
    }
    toggle->setChecked(true);
    if (!wait_until([&] { return screenshot->isEnabled(); }))
      return false;
    const QString path = QDir(artifacts).filePath(QString::fromLatin1(name) + ".png");
    QString error;
    if (!dialog.captureScreenshot(path, &error)) {
      std::cerr << "Overlay capture failed: " << error.toStdString() << '\n';
      return false;
    }
    const QImage drawn = QImage(path).copy(video_rect);
    int different = 0;
    for (int y = 0; y < clean.height(); ++y)
      for (int x = 0; x < clean.width(); ++x)
        different += clean.pixel(x, y) != drawn.pixel(x, y);
    if (different < 50 || timeline->value() != comparison_frame) {
      std::cerr << "Paused overlay did not draw on the same GPU frame: " << name << " changed=" << different << '\n';
      return false;
    }
    std::cout << name << " changed " << different << " preview pixels on paused frame " << comparison_frame << '\n';
    toggle->setChecked(false);
    if (!wait_until([&] { return screenshot->isEnabled(); }))
      return false;
  }
  // The plot paints on the CPU, so its layers can be checked directly: flipping
  // one must change what the plot draws on the same paused frame.
  for (const char* name : {"experimentPlotLayer_Recorded",
                           "experimentPlotLayer_Program",
                           "experimentPlotLayer_Crop",
                           "experimentPlotLayer_Fast",
                           "experimentPlotLayer_Players",
                           "experimentPlotLayer_Cluster",
                           "experimentPlotLayer_Breakaway"}) {
    auto* toggle = widget<QCheckBox>(dialog, name);
    const QImage before = plot->grab().toImage();
    toggle->toggle();
    QCoreApplication::processEvents();
    const QImage after = plot->grab().toImage();
    toggle->toggle();
    int different = 0;
    for (int y = 0; y < before.height(); ++y)
      for (int x = 0; x < before.width(); ++x)
        different += before.pixel(x, y) != after.pixel(x, y);
    std::cout << name << " changed " << different << " plot pixels\n";
    // Program center and crop width exist on every prepared frame. The rest
    // depend on what the clip contains, so their counts are only reported.
    const std::string layer(name);
    if (different == 0 && (layer == "experimentPlotLayer_Program" || layer == "experimentPlotLayer_Crop")) {
      std::cerr << "Camera plot layer drew nothing: " << name << '\n';
      return false;
    }
  }
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
  widget<QPushButton>(dialog, "experimentStopPreview")->click();
  if (!wait_until([&] { return play->isEnabled(); }) || !click_plot_frame(comparison_frame))
    return false;
  auto* in = widget<QDoubleSpinBox>(dialog, "experimentIn");
  in->setValue(in->value() + 1);
  if (!in->isEnabled()) {
    std::cerr << "Stopping preview interrupted editing the recording range\n";
    return false;
  }
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
      << "PASS: historical preparation, parameter trial, actual GPU playback, visible paused resize/maximize, visual timeline seek/resume, paused frame step, original comparison, range loop, end pause, screenshots, stale trial save invalidation\n";
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
    if (app.arguments().contains("--rink-comparison"))
      return rink_comparison(app.arguments()) ? 0 : 1;
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
