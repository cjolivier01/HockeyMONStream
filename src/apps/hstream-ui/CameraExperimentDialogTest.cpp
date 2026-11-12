#include "src/apps/hstream-ui/CameraExperimentDialog.h"
#include "src/apps/hstream-ui/CameraControlSpecs.h"
#include "src/apps/hstream-ui/CameraExperimentCaptureTest.h"
#include "src/apps/hstream-ui/CameraExperimentPreviewWorker.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtGui/QImageReader>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSizeGrip>
#include <QtWidgets/QSlider>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QToolButton>

#include <yaml-cpp/yaml.h>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <optional>
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

bool save_trial_dialog(QPushButton* button, const QString& path, bool accept) {
  bool seen = false, handled = false;
  QTimer poll, deadline;
  QObject::connect(&poll, &QTimer::timeout, [&] {
    auto* chooser = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
    if (!chooser || seen)
      return;
    seen = true;
    if (chooser->acceptMode() != QFileDialog::AcceptSave) {
      chooser->reject();
      return;
    }
    if (accept) {
      chooser->selectFile(path);
      handled = QMetaObject::invokeMethod(chooser, "accept", Qt::DirectConnection);
    } else {
      chooser->reject();
      handled = true;
    }
  });
  deadline.setSingleShot(true);
  QObject::connect(&deadline, &QTimer::timeout, [&] {
    handled = false;
    if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
      modal->reject();
  });
  poll.start(10);
  deadline.start(5000);
  button->click();
  return seen && handled;
}

bool close_save_probe(CameraExperimentDialog& dialog, const QString& path, const QString& expected_path) {
  const QFileInfo destination(path);
  const QDir parent = destination.dir();
  const QStringList partial_pattern{destination.fileName() + ".tmp-*"};
  auto* save = widget<QPushButton>(dialog, "experimentSave");
  auto* cancel = widget<QPushButton>(dialog, "experimentCancel");
  const QString selected_trial = widget<QComboBox>(dialog, "experimentComparison")->currentText();
  if (QFileInfo::exists(path) || !parent.entryList(partial_pattern, QDir::Files).isEmpty() ||
      !save_trial_dialog(save, path, true))
    throw std::runtime_error("Cannot start the independent Close-during-Save probe");
  const bool admitted = !save->isEnabled() && cancel->isEnabled() && cancel->text() == "Cancel saving";
  QElapsedTimer elapsed;
  elapsed.start();
  QStringList partials;
  while (elapsed.elapsed() < 60000) {
    QCoreApplication::processEvents();
    partials = parent.entryList(partial_pattern, QDir::Files);
    if (!partials.isEmpty() || save->isEnabled() || QFileInfo::exists(path))
      break;
    QThread::msleep(1);
  }
  const bool committed_before_close = QFileInfo::exists(path);
  const bool pending_before_close = !save->isEnabled();
  if (!pending_before_close && !committed_before_close)
    throw std::runtime_error("Save ended without an output before the Close probe could act");
  const bool uncommitted_output_observed = !partials.isEmpty() && !committed_before_close;
  const qint64 before_close_ms = elapsed.elapsed();
  dialog.reject();
  const bool visible_closing = dialog.isVisible();
  if (pending_before_close && !visible_closing)
    throw std::runtime_error("Close did not remain visible while its Save future was outstanding");
  if (!wait_until([&] { return !dialog.isVisible(); }, 60000))
    throw std::runtime_error("Close-during-Save did not drain background work");
  QElapsedTimer retired;
  retired.start();
  while (retired.elapsed() < 100) {
    QCoreApplication::processEvents();
    if (dialog.isVisible())
      throw std::runtime_error("A late Save completion reopened Camera");
    QThread::msleep(1);
  }
  if (!parent.entryList(partial_pattern, QDir::Files).isEmpty())
    throw std::runtime_error("Close-during-Save retained a partial output");
  if (widget<QComboBox>(dialog, "experimentComparison")->currentText() != selected_trial)
    throw std::runtime_error("Close-during-Save changed the selected trial");
  const bool committed = QFileInfo::exists(path);
  if (committed) {
    const auto digest = [](const QString& source) {
      QFile file(source);
      if (!file.open(QIODevice::ReadOnly) || file.size() > 128 * 1024 * 1024)
        throw std::runtime_error("Close/Save output is unavailable or too large");
      QCryptographicHash hash(QCryptographicHash::Sha256);
      if (!hash.addData(&file))
        throw std::runtime_error("Cannot verify committed Close/Save output");
      return hash.result();
    };
    if (digest(path) != digest(expected_path))
      throw std::runtime_error("A Save committed during Close but differs from the prepared trial");
  }
  camera_capture_test::write_json(
      path + ".close.json",
      {{"ui_save_admitted", admitted},
       {"outstanding_ui_future_before_close", pending_before_close},
       {"temporary_outputs_observed", QJsonArray::fromStringList(partials)},
       {"uncommitted_output_observed", uncommitted_output_observed},
       {"committed_before_close", committed_before_close},
       {"visible_while_closing", visible_closing},
       {"committed_after_close", committed},
       {"partial_output_remaining", false},
       {"late_reopen", false},
       {"wait_before_close_ms", before_close_ms},
       {"serialization_boundary", "unrun: no serializer barrier; UI admission is not evidence of active serialization"},
       {"output_boundary",
        uncommitted_output_observed ? "observed temporary output before Close; commit may race"
                                    : "unrun: no uncommitted output observed before Close"},
       {"outcome", committed ? "committed result validated" : "no committed or partial output"}});
  std::cout << "Close Save: " << (committed ? "validated committed result" : "no output")
            << "; uncommitted temporary output observed=" << uncommitted_output_observed
            << "; active serialization boundary UNRUN (no barrier)\n";
  return true;
}

bool screenshot_export_inventory() {
  namespace fs = std::filesystem;
  QTemporaryDir temporary;
  if (!temporary.isValid())
    return false;
  const fs::path root(temporary.path().toStdString());
  const auto game = root / "real/Game";
  const auto alias = root / "alias/Game";
  const auto external = root / "external";
  fs::create_directories(game / "nested");
  fs::create_directories(alias.parent_path());
  fs::create_directories(external);
  fs::create_directory_symlink(game, alias);
  CameraExperimentDialog dialog(QString::fromStdString(alias.string()));
  dialog.show();
  QCoreApplication::processEvents();
  const auto capture = [&](const fs::path& path) {
    QString error;
    if (!dialog.captureScreenshot(QString::fromStdString(path.string()), &error)) {
      std::cerr << "Screenshot export failed: " << error.toStdString() << '\n';
      return false;
    }
    return !QImage(QString::fromStdString(path.string())).isNull();
  };
  const auto manifest = [&] { return YAML::LoadFile((game / "_hockeymon/manifest.json").string()); };
  const auto inventoried = [&](const std::string& path, const std::string& role) {
    for (const auto& entry : manifest()["files"])
      if (entry["path"].as<std::string>() == path)
        return entry["role"].as<std::string>() == role && entry["kind"].as<std::string>() == "file" &&
            entry["size_bytes"].as<uint64_t>() == fs::file_size(game / path);
    std::cerr << "Screenshot export is missing from game inventory: " << path << '\n';
    return false;
  };
  // File choosers may return the physical filename while the selected game is
  // an alias. The screenshot must still belong to that game's inventory.
  if (!capture(game / "physical.png") || !inventoried("physical.png", "published"))
    return false;
  widget<QLineEdit>(dialog, "experimentGameDirectory")->setText(QString::fromStdString(game.string()));
  if (!capture(game / "nested/../parent.png") || !inventoried("parent.png", "published"))
    return false;
  fs::create_directories(game / "_hockeymon/exports");
  fs::create_directory_symlink(game / "_hockeymon/exports", game / "private-export");
  if (!capture(game / "private-export/private.png") || !inventoried("_hockeymon/exports/private.png", "saved-state"))
    return false;
  fs::create_directory_symlink(external, game / "external-export");
  const auto before_external = YAML::Dump(manifest());
  if (!capture(game / "external-export/outside.png") || !fs::is_regular_file(external / "outside.png") ||
      YAML::Dump(manifest()) != before_external) {
    std::cerr << "Export through an external directory alias changed game inventory\n";
    return false;
  }
  // A vanished selected game is reported after the screenshot is saved; no
  // filesystem exception may escape the same capture path used by the Qt slot.
  widget<QLineEdit>(dialog, "experimentGameDirectory")->setText(QString::fromStdString((root / "missing").string()));
  QString error;
  const auto saved = external / "missing-game.png";
  if (dialog.captureScreenshot(QString::fromStdString(saved.string()), &error) ||
      !error.contains("Screenshot saved, but game inventory failed") || !fs::is_regular_file(saved))
    return false;
  return true;
}

bool legacy_source_controls() {
  QTemporaryDir directory;
  if (!directory.isValid())
    return false;
  CameraExperimentDialog dialog;
  auto* manifest = widget<QLineEdit>(dialog, "experimentManifest");
  auto* arena = widget<QLineEdit>(dialog, "experimentLegacyArena");
  auto* config = widget<QLineEdit>(dialog, "experimentLegacyConfig");
  auto* prepare = widget<QPushButton>(dialog, "experimentPrepare");
  auto* status = widget<QLabel>(dialog, "experimentStatus");
  manifest->clear();
  if (arena->isEnabled() || config->isEnabled())
    return false;
  const QString legacy = directory.filePath("hstream_telemetry.json");
  manifest->setText(legacy);
  if (!arena->isEnabled() || !config->isEnabled())
    return false;
  arena->setText("unfinished arena draft");
  config->setText(directory.filePath("historical.yaml"));
  for (const QString& extension : {QString("db"), QString("sqlite")}) {
    manifest->setText(directory.filePath("recording." + extension));
    if (arena->isEnabled() || config->isEnabled()) {
      std::cerr << "SQLite recording exposes unused Legacy controls\n";
      return false;
    }
    prepare->click();
    // The missing database must reach native preparation, rather than fail
    // immediately on the unrelated retained arena draft.
    if (prepare->isEnabled() || !wait_until([&] { return prepare->isEnabled(); }, 5000) ||
        status->text().contains("Historical arena")) {
      std::cerr << "SQLite preparation validated an irrelevant Legacy arena\n";
      return false;
    }
  }
  manifest->setText(legacy);
  if (!arena->isEnabled() || !config->isEnabled() || arena->text() != "unfinished arena draft" ||
      config->text() != directory.filePath("historical.yaml")) {
    std::cerr << "Switching recording kinds lost the Legacy drafts\n";
    return false;
  }
  prepare->click();
  if (!prepare->isEnabled() || !status->text().contains("Historical arena")) {
    std::cerr << "Legacy preparation skipped its arena validation\n";
    return false;
  }
  return true;
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
  const auto usable_geometry = [&] {
    auto* plot = widget<QWidget>(first, "experimentCameraPlot");
    auto* pane = widget<QWidget>(first, "experimentControlsPanel");
    auto* target = widget<QWidget>(first, "experimentVideoTarget");
    const QPoint preview_at = target->mapTo(&first, QPoint());
    const QPoint controls_at = pane->mapTo(&first, QPoint());
    const bool usable = plot->isVisible() && pane->isVisible() && plot->height() >= 190 && pane->width() >= 390 &&
        target->width() >= 480 && target->height() >= 270 && controls_at.x() >= preview_at.x() + target->width();
    if (!usable)
      std::cerr << "Camera plot/pane/preview geometry is unusable: plot=" << plot->width() << 'x' << plot->height()
                << " pane=" << pane->width() << 'x' << pane->height() << " preview=" << target->width() << 'x'
                << target->height() << '\n';
    return usable;
  };
  if (!usable_geometry())
    return false;
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
  if (!usable_geometry())
    return false;
  maximize_window->click();
  if (!wait_until([&] { return first.isMaximized() && maximize_window->text() == "Restore window"; }, 2000)) {
    std::cerr << "Camera experiments window did not maximize\n";
    return false;
  }
  if (!usable_geometry())
    return false;
  maximize_window->click();
  if (!wait_until([&] { return !first.isMaximized() && maximize_window->text() == "Maximize window"; }, 2000)) {
    std::cerr << "Camera experiments window did not restore\n";
    return false;
  }
  if (!usable_geometry())
    return false;
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
  if (manifest.endsWith(".db") || manifest.endsWith(".sqlite")) {
    // Retained Legacy drafts must not prevent a valid database preparation.
    widget<QLineEdit>(dialog, "experimentLegacyArena")->setText("unfinished arena draft");
    widget<QLineEdit>(dialog, "experimentLegacyConfig")->setText("/missing/historical.yaml");
  }
  widget<QDoubleSpinBox>(dialog, "experimentDuration")->setValue(1);
  auto* prepare = widget<QPushButton>(dialog, "experimentPrepare");
  auto* apply = widget<QPushButton>(dialog, "experimentApply");
  prepare->click();
  if (!wait_until([&]() { return prepare->isEnabled(); }) || !apply->isEnabled() || !require_confirmation(dialog))
    return false;
  if (manifest.endsWith(".db") || manifest.endsWith(".sqlite")) {
    widget<QLineEdit>(dialog, "experimentLegacyArena")->clear();
    widget<QLineEdit>(dialog, "experimentLegacyConfig")->clear();
    if (!apply->isEnabled()) {
      std::cerr << "Inactive Legacy drafts invalidated the prepared database\n";
      return false;
    }
  }
  apply->click();
  if (!wait_until([&]() { return prepare->isEnabled(); }) ||
      widget<QComboBox>(dialog, "experimentComparison")->count() != 3 ||
      widget<QLineEdit>(dialog, "experimentTrialName")->text() != "Trial 2") {
    std::cerr << "Confirming the source did not allow the trial to be calculated\n";
    return false;
  }
  auto* save = widget<QPushButton>(dialog, "experimentSave");
  auto* confirmed = widget<QCheckBox>(dialog, "experimentUncropped");
  auto* comparison = widget<QComboBox>(dialog, "experimentComparison");
  if (!save->isEnabled())
    return false;
  confirmed->setChecked(false);
  comparison->setCurrentIndex(0);
  comparison->setCurrentIndex(2);
  if (!wait_until([&] { return prepare->isEnabled(); }) || save->isEnabled()) {
    std::cerr << "Revoking source confirmation left the existing trial saveable\n";
    return false;
  }
  confirmed->setChecked(true);
  if (!wait_until([&] { return save->isEnabled(); })) {
    std::cerr << "Restoring source confirmation did not enable the existing trial\n";
    return false;
  }
  std::cout << "PASS: confirmation gates trial creation and saving, including revocation and reselection\n";
  return true;
}

// Exercise the real dense serializer without requiring a video renderer. This
// isolates Save/Cancel/Close responsiveness from compositor and decoder checks.
bool save_trial_workflow(QStringList args) {
  const bool observe_close = args.removeAll("--close-during-save") > 0;
  if (args.size() != 7 || !QDir().mkpath(args[3])) {
    std::cerr << "Usage: --save-trial MANIFEST ARTIFACT_DIR DURATION ARENA CONFIG\n";
    return false;
  }
  CameraExperimentDialog dialog;
  dialog.show();
  widget<QLineEdit>(dialog, "experimentManifest")->setText(args[2]);
  widget<QDoubleSpinBox>(dialog, "experimentDuration")->setValue(args[4].toDouble());
  widget<QLineEdit>(dialog, "experimentLegacyArena")->setText(args[5]);
  widget<QLineEdit>(dialog, "experimentLegacyConfig")->setText(args[6]);
  widget<QCheckBox>(dialog, "experimentUncropped")->setChecked(true);
  auto* prepare = widget<QPushButton>(dialog, "experimentPrepare");
  auto* apply = widget<QPushButton>(dialog, "experimentApply");
  auto* save = widget<QPushButton>(dialog, "experimentSave");
  auto* cancel = widget<QPushButton>(dialog, "experimentCancel");
  auto* status = widget<QLabel>(dialog, "experimentStatus");
  auto* comparison = widget<QComboBox>(dialog, "experimentComparison");
  prepare->click();
  if (!wait_until([&] { return prepare->isEnabled(); }) || !apply->isEnabled()) {
    std::cerr << "Save fixture preparation failed: " << status->text().toStdString() << '\n';
    return false;
  }
  apply->click();
  if (!wait_until([&] { return save->isEnabled(); }))
    return false;
  const int frames = widget<QSlider>(dialog, "experimentTimeline")->maximum() + 1;
  const QString trial = comparison->currentText();
  QElapsedTimer heartbeat;
  heartbeat.start();
  qint64 longest_gap = 0;
  QTimer responsiveness;
  QObject::connect(
      &responsiveness, &QTimer::timeout, [&] { longest_gap = std::max(longest_gap, heartbeat.restart()); });
  responsiveness.start(10);
  const auto path = [&](const char* name) { return QDir(args[3]).absoluteFilePath(name); };
  if (!save_trial_dialog(save, path("chooser-cancelled.yaml"), false) ||
      QFileInfo::exists(path("chooser-cancelled.yaml")) || !save->isEnabled())
    return false;
  if (!save_trial_dialog(save, path("cancelled.yaml"), true) || save->isEnabled() || !cancel->isEnabled() ||
      cancel->text() != "Cancel saving")
    return false;
  cancel->click();
  if (!wait_until([&] { return save->isEnabled(); }) || QFileInfo::exists(path("cancelled.yaml")) ||
      !status->text().contains("Saving cancelled") || comparison->currentText() != trial)
    return false;
  if (!save_trial_dialog(save, path("saved.yaml"), true) || !wait_until([&] { return save->isEnabled(); }) ||
      !QFileInfo::exists(path("saved.yaml")) || !status->text().contains("Trial saved without a video binding"))
    return false;
  if (observe_close) {
    if (!close_save_probe(dialog, path("closed.yaml"), path("saved.yaml")))
      return false;
  } else {
    if (!save_trial_dialog(save, path("closed.yaml"), true) || save->isEnabled())
      return false;
    dialog.reject();
    if (!wait_until([&] { return !dialog.isVisible(); }) || QFileInfo::exists(path("closed.yaml")))
      return false;
  }
  responsiveness.stop();
  longest_gap = std::max(longest_gap, heartbeat.elapsed());
  const auto exported = YAML::LoadFile(path("saved.yaml").toStdString());
  const bool intact = exported["schema"].as<std::string>() == "hstream-camera-experiment-v1" &&
      exported["name"].as<std::string>() == trial.toStdString() &&
      exported["frames"].size() == static_cast<std::size_t>(frames) &&
      exported["media"]["kind"].as<std::string>() == "none";
  std::cout << "Save/Cancel/Close: " << frames << " frames; longest UI heartbeat gap: " << longest_gap << " ms\n";
  return intact && longest_gap < 1000;
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

QJsonObject camera_control_snapshot(CameraExperimentDialog& dialog) {
  QJsonArray controls;
  const auto unused = [](const char*) { return 0; };
  for (const auto& group :
       {tracking_control_specs(unused), motion_control_specs(unused), player_control_specs(unused)}) {
    for (const auto& spec : group) {
      if (!spec.runtime_key)
        continue;
      const QByteArray id(spec.id);
      auto* value = widget<QDoubleSpinBox>(dialog, ("experimentValue_" + id).constData());
      auto* enabled = widget<QCheckBox>(dialog, ("experimentOverride_" + id).constData());
      controls.append(
          QJsonObject{
              {"id", spec.id},
              {"raw_value", value->value() * spec.divisor},
              {"value", value->value()},
              {"divisor", spec.divisor},
              {"override_checked", enabled->isChecked()},
              {"override_enabled", enabled->isEnabled()},
              {"value_enabled", value->isEnabled()}});
    }
  }
  return {
      {"controls", controls},
      {"fast", widget<QCheckBox>(dialog, "experimentFastBox")->isChecked()},
      {"follower", widget<QCheckBox>(dialog, "experimentFollowerBox")->isChecked()},
      {"playback_start_ms", widget<QDoubleSpinBox>(dialog, "experimentPlaybackStart")->value() * 1000}};
}

int confirmed_camera_frame(CameraExperimentDialog& dialog) {
  return camera_capture_test::confirmed_frame(
      widget<QLabel>(dialog, "experimentFrameStatus")->text(),
      widget<QSlider>(dialog, "experimentTimeline")->maximum() + 1,
      widget<QPushButton>(dialog, "experimentScreenshot")->isEnabled());
}

bool e2e(const QStringList& raw_args) {
  QStringList args;
  // Optional test-driver probes leave ordinary e2e coverage unchanged.
  int requested_overlay_frame = -1, expected_frame_count = -1;
  QList<int> exact_seek_frames;
  QString handshake_directory, control_snapshot_path;
  bool close_during_save = false;
  for (const QString& argument : raw_args) {
    const auto parse_nonnegative = [](const QString& value) {
      bool valid = false;
      const int number = value.toInt(&valid);
      if (!valid || number < 0)
        throw std::runtime_error("Expected a nonnegative test frame/count");
      return number;
    };
    if (argument.startsWith("--capture-handshake-dir="))
      handshake_directory = argument.mid(QString("--capture-handshake-dir=").size());
    else if (argument.startsWith("--control-snapshot="))
      control_snapshot_path = argument.mid(QString("--control-snapshot=").size());
    else if (argument == "--close-during-save")
      close_during_save = true;
    else if (argument.startsWith("--overlay-frame="))
      requested_overlay_frame = parse_nonnegative(argument.mid(QString("--overlay-frame=").size()));
    else if (argument.startsWith("--expect-frame-count="))
      expected_frame_count = parse_nonnegative(argument.mid(QString("--expect-frame-count=").size()));
    else if (argument.startsWith("--exact-seek-frames=")) {
      for (const QString& value : argument.mid(QString("--exact-seek-frames=").size()).split(','))
        exact_seek_frames.append(parse_nonnegative(value));
    } else
      args.append(argument);
  }
  if (args.size() < 8) {
    std::cerr
        << "Usage: --e2e MANIFEST PANORAMA ARTIFACT_DIR OFFSET DURATION VIDEO_OFFSET [ARENA] [CONFIG] [PLAYBACK_START] [--overlay-frame=N] [--exact-seek-frames=N,...] [--expect-frame-count=N] [--capture-handshake-dir=DIR] [--control-snapshot=JSON] [--close-during-save]\n";
    return false;
  }
  const QString artifacts = args[4];
  if (!QDir().mkpath(artifacts))
    return false;
  camera_capture_test::Handshake handshake(handshake_directory);
  const auto initial = control_snapshot_path.isEmpty() ? camera_capture_test::Controls{}
                                                       : camera_capture_test::read_controls(control_snapshot_path);
  const qint64 playback_ms = args.size() > 10 ? std::llround(args[10].toDouble() * 1000) : initial.playback_start_ms;
  if (!control_snapshot_path.isEmpty() && playback_ms != initial.playback_start_ms)
    throw std::runtime_error("Positional Playback start differs from the supplied control snapshot");
  QElapsedTimer heartbeat;
  heartbeat.start();
  qint64 longest_gap = 0;
  QTimer responsiveness;
  const auto record_heartbeat = [&] { longest_gap = std::max(longest_gap, heartbeat.restart()); };
  QObject::connect(&responsiveness, &QTimer::timeout, record_heartbeat);
  responsiveness.start(10);
  CameraExperimentDialog dialog(QFileInfo(args[2]).absolutePath(), nullptr, initial.values, playback_ms);
  const auto constructor_controls = camera_control_snapshot(dialog);
  if (constructor_controls.value("playback_start_ms").toDouble() != playback_ms)
    throw std::runtime_error("The supplied Playback start cannot be represented by the Camera control");
  for (const auto& target :
       {std::make_pair("Apply_To_Fast_Box", "fast"), std::make_pair("Apply_To_Follower_Box", "follower")}) {
    const auto supplied = initial.values.find(target.first);
    if (supplied != initial.values.end() &&
        constructor_controls.value(target.second).toBool() != (supplied->second != 0))
      throw std::runtime_error("Camera did not inherit the supplied target choice");
  }
  for (const auto& item : constructor_controls.value("controls").toArray()) {
    const auto actual = item.toObject();
    const auto supplied = initial.values.find(actual.value("id").toString());
    if (supplied != initial.values.end() &&
        (!actual.value("override_checked").toBool() ||
         std::abs(actual.value("raw_value").toDouble() - supplied->second) >
             1e-12 * std::max(1.0, std::abs(supplied->second))))
      throw std::runtime_error("The initial control snapshot cannot be represented without changing its raw value");
  }
  camera_capture_test::write_json(
      QDir(artifacts).filePath("control-context-initial.json"),
      {{"supplied_path", control_snapshot_path},
       {"complete_applicable_map", initial.complete},
       {"actual", constructor_controls}});
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
  for (auto* control : dialog.findChildren<QCheckBox*>())
    if (control->objectName().startsWith("experimentOverride_"))
      control->setChecked(false);
  widget<QCheckBox>(dialog, "experimentFastBox")->setChecked(false);
  widget<QCheckBox>(dialog, "experimentFollowerBox")->setChecked(true);
  widget<QCheckBox>(dialog, "experimentOverride_Max_Speed_X_x10")->setChecked(true);
  widget<QDoubleSpinBox>(dialog, "experimentValue_Max_Speed_X_x10")->setValue(1.5);
  widget<QCheckBox>(dialog, "experimentOverride_Max_Accel_X_x10")->setChecked(true);
  widget<QDoubleSpinBox>(dialog, "experimentValue_Max_Accel_X_x10")->setValue(0.2);
  widget<QLineEdit>(dialog, "experimentTrialName")->setText("A · slower pan");
  camera_capture_test::write_json(
      QDir(artifacts).filePath("control-context-trial.json"), camera_control_snapshot(dialog));
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
  std::optional<YAML::Node> reference_trial;
  QString reference_sha256, reference_path;
  if (handshake.enabled()) {
    // A real, separate Save binds the trial's exact immutable sample/PTS rows
    // without adding an accessor or GPU-generation claim to production code.
    reference_path = QDir(artifacts).absoluteFilePath("capture-reference-trial.yaml");
    auto* reference_save = widget<QPushButton>(dialog, "experimentSave");
    auto* reference_play = widget<QPushButton>(dialog, "experimentPlay");
    if (reference_play->text() == "Pause")
      reference_play->click();
    if (!wait_until([&] {
          return reference_play->text() != "Pause" && reference_save->isEnabled() &&
              confirmed_camera_frame(dialog) >= 0;
        }))
      throw std::runtime_error("Capture reference Save did not reach a confirmed paused frame");
    const int paused_reference_frame = confirmed_camera_frame(dialog);
    if (QFileInfo::exists(reference_path) || !save_trial_dialog(reference_save, reference_path, true) ||
        !wait_until([&] { return reference_save->isEnabled(); }) || !QFileInfo::exists(reference_path))
      throw std::runtime_error("Could not save the independent capture reference trial");
    QFile reference_file(reference_path);
    if (!reference_file.open(QIODevice::ReadOnly) || reference_file.size() > 128 * 1024 * 1024)
      throw std::runtime_error("Capture reference trial is unavailable or too large");
    QCryptographicHash digest(QCryptographicHash::Sha256);
    if (!digest.addData(&reference_file))
      throw std::runtime_error("Could not hash the capture reference trial");
    reference_sha256 = QString::fromLatin1(digest.result().toHex());
    reference_trial.emplace(YAML::LoadFile(reference_path.toStdString()));
    if ((*reference_trial)["schema"].as<std::string>() != "hstream-camera-experiment-v1" ||
        (*reference_trial)["frames"].size() !=
            static_cast<std::size_t>(widget<QSlider>(dialog, "experimentTimeline")->maximum() + 1))
      throw std::runtime_error("Capture reference trial differs from the prepared sample range");
    heartbeat.restart(); // Parsing/hash work is test-only; the real Save was measured.
    reference_play->click();
    if (!wait_until([&] {
          const int frame = confirmed_camera_frame(dialog);
          return reference_play->text() == "Pause" && frame >= 0 && frame != paused_reference_frame;
        }))
      throw std::runtime_error("Capture reference Save did not resume verified frame progress");
    camera_capture_test::write_json(
        QDir(artifacts).filePath("capture-reference-playback.json"),
        {{"paused_frame", paused_reference_frame},
         {"resumed_frame", confirmed_camera_frame(dialog)},
         {"playing_after_save", reference_play->text() == "Pause"},
         {"save_enabled_after_resume", reference_save->isEnabled()}});
  }
  const auto bounds = [&](const char* object_name) {
    auto* control = widget<QWidget>(dialog, object_name);
    const QPoint origin = control->mapTo(&dialog, QPoint());
    return QJsonArray{origin.x(), origin.y(), control->width(), control->height()};
  };
  const auto observation = [&] {
    const QPoint origin = dialog.mapToGlobal(QPoint());
    const int confirmed = confirmed_camera_frame(dialog);
    QJsonArray scroll;
    for (auto* bar : dialog.findChildren<QScrollBar*>())
      scroll.append(
          QJsonObject{
              {"name", bar->objectName()},
              {"orientation", int(bar->orientation())},
              {"value", bar->value()},
              {"maximum", bar->maximum()}});
    QJsonObject value{
        {"device_pixel_ratio", dialog.devicePixelRatioF()},
        {"dialog_global_origin", QJsonArray{origin.x(), origin.y()}},
        {"dialog_size", QJsonArray{dialog.width(), dialog.height()}},
        {"preview", bounds("experimentVideoTarget")},
        {"timeline", bounds("experimentCameraPlot")},
        {"control_tabs", bounds("experimentControlTabs")},
        {"tab", widget<QTabWidget>(dialog, "experimentControlTabs")->currentIndex()},
        {"controls", camera_control_snapshot(dialog)},
        {"scroll", scroll},
        {"view", comparison->currentIndex()},
        {"view_name", comparison->currentText()},
        {"manifest", widget<QLineEdit>(dialog, "experimentManifest")->text()},
        {"media", widget<QLineEdit>(dialog, "experimentMedia")->text()},
        {"playing", widget<QPushButton>(dialog, "experimentPlay")->text() == "Pause"},
        {"requested_index", widget<QSlider>(dialog, "experimentTimeline")->value()},
        {"confirmed_index", confirmed},
        {"frame_label", widget<QLabel>(dialog, "experimentFrameStatus")->text()},
        {"screenshot_ready", screenshot->isEnabled()}};
    for (const char* name : {"experimentDrawPlayers", "experimentDrawPlayTracking", "experimentDrawRinkMask"})
      value.insert(name, widget<QCheckBox>(dialog, name)->isChecked());
    QJsonObject layers;
    for (auto* toggle : dialog.findChildren<QCheckBox*>())
      if (toggle->objectName().startsWith("experimentPlotLayer_"))
        layers.insert(toggle->objectName(), toggle->isChecked());
    value.insert("plot_layers", layers);
    if (reference_trial && confirmed >= 0) {
      const auto row = (*reference_trial)["frames"][confirmed];
      value.insert("trial_sample_id", QString::number(row["sample_id"].as<uint64_t>()));
      value.insert("trial_pts_ns", QString::number(row["pts_ns"].as<uint64_t>()));
    }
    return value;
  };
  const auto capture_identity = [](QJsonObject value) {
    if (value.value("playing").toBool())
      for (const char* key :
           {"requested_index", "confirmed_index", "frame_label", "screenshot_ready", "trial_sample_id", "trial_pts_ns"})
        value.remove(key);
    return value;
  };
  const auto stable_observation = [&] { return capture_identity(observation()); };
  const auto capture = [&](const char* name, bool handoff = true) {
    QJsonObject before;
    if (handshake.enabled() && handoff) {
      QJsonObject prior;
      if (!wait_until(
              [&] {
                const auto now = stable_observation();
                const bool stable = !prior.isEmpty() && now == prior && confirmed_camera_frame(dialog) >= 0;
                prior = now;
                return stable;
              },
              2000))
        throw std::runtime_error("Camera capture state did not settle");
      before = observation();
      if (!before.value("playing").toBool() && before.value("requested_index") != before.value("confirmed_index"))
        throw std::runtime_error("Paused requested and Poll-confirmed frames differ");
      handshake.begin(
          QFileInfo(QString::fromLatin1(name)).completeBaseName().toLower(),
          {{"state", before}, {"image", QDir(artifacts).absoluteFilePath(name)}});
    }
    // GPU readback plus full-dialog PNG encoding is an explicit test-driver
    // operation. Count any application gap before it, then exclude only that
    // synchronous capture from the playback responsiveness measurement.
    record_heartbeat();
    QElapsedTimer screenshot_time;
    screenshot_time.start();
    QString error;
    const bool captured = dialog.captureScreenshot(QDir(artifacts).filePath(name), &error);
    heartbeat.restart();
    if (!captured) {
      std::cerr << error.toStdString() << '\n';
      return false;
    }
    const QSize pixels = QImageReader(QDir(artifacts).filePath(name)).size();
    QJsonObject geometry{
        {"device_pixel_ratio", dialog.devicePixelRatioF()},
        {"dialog_size", QJsonArray{dialog.width(), dialog.height()}},
        {"minimum_size", QJsonArray{dialog.minimumWidth(), dialog.minimumHeight()}},
        {"image_size", QJsonArray{pixels.width(), pixels.height()}},
        {"preview", bounds("experimentVideoTarget")},
        {"timeline", bounds("experimentCameraPlot")},
        {"control_tabs", bounds("experimentControlTabs")}};
    const auto after = observation();
    geometry.insert("before_capture", before);
    geometry.insert("after_capture", after);
    geometry.insert("synchronous_capture_ms", screenshot_time.elapsed());
    camera_capture_test::write_json(QDir(artifacts).filePath(QString(name) + ".geometry.json"), geometry);
    std::cout << "Test screenshot " << name << " took " << screenshot_time.elapsed() << " ms\n";
    if (handshake.enabled() && handoff) {
      if (capture_identity(before) != capture_identity(after))
        throw std::runtime_error("Camera checkpoint identity changed during internal capture");
      geometry.insert("reference_trial", reference_path);
      geometry.insert("reference_sha256", reference_sha256);
      geometry.insert("image", QDir(artifacts).absoluteFilePath(name));
      geometry.insert(
          "frame_evidence",
          "Poll-derived UI label/readiness plus immutable trial sample/PTS; no GPU-generation receipt");
      geometry.insert(
          "motion_scope",
          before.value("playing").toBool()
              ? "Playing capture records an observed frame interval; compositor frame identity requires independent review"
              : "Paused index and geometry remain stable through the acknowledged external capture");
      const QString phase = QFileInfo(QString::fromLatin1(name)).completeBaseName().toLower();
      handshake.checkpoint(phase, geometry, stable_observation);
      camera_capture_test::write_json(QDir(artifacts).filePath(phase + ".after-ack.json"), observation());
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
  if (handshake.enabled() && !args.contains("--unequal-cadence")) {
    const QSize normal = dialog.size();
    const auto playing_capture = [&](const char* phase) {
      QElapsedTimer settling;
      settling.start();
      if (!wait_until(
              [&] { return settling.elapsed() >= 200 && screenshot->isEnabled() && play->text() == "Pause"; }, 5000))
        throw std::runtime_error("Playing Camera geometry did not settle with a ready frame");
      return capture(phase);
    };
    if (!playing_capture("playing-normal.png"))
      return false;
    dialog.resize(normal + QSize(100, 80));
    if (!playing_capture("playing-resized.png"))
      return false;
    dialog.resize(normal);
    if (!playing_capture("playing-resize-restored.png"))
      return false;
    auto* focus = widget<QToolButton>(dialog, "maximizeCameraExperimentPreviewButton");
    focus->click();
    if (!playing_capture("playing-preview-maximized.png"))
      return false;
    focus->click();
    if (!playing_capture("playing-preview-restored.png"))
      return false;
    auto* maximize = widget<QToolButton>(dialog, "maximizeCameraExperimentWindowButton");
    maximize->click();
    if (!playing_capture("playing-window-maximized.png"))
      return false;
    maximize->click();
    if (!playing_capture("playing-window-restored.png"))
      return false;
  }
  if (args.contains("--unequal-cadence")) {
    auto* timeline = widget<QSlider>(dialog, "experimentTimeline");
    int previous = timeline->value();
    if (!wait_until(
            [&] {
              const int current = timeline->value();
              const bool wrapped = previous > timeline->maximum() / 2 && current < previous;
              previous = current;
              return wrapped;
            },
            15000)) {
      std::cerr << "Unequal-cadence replay did not loop on its actual final decoded image\n";
      return false;
    }
    widget<QCheckBox>(dialog, "experimentLoop")->setChecked(false);
    if (!wait_until([&] { return play->text() != "Pause" && screenshot->isEnabled(); }, 15000) ||
        timeline->value() >= timeline->maximum() || status->styleSheet().contains("#b42318") ||
        !capture("unequal-cadence-end.png")) {
      std::cerr << "Unequal-cadence replay did not pause at its actual final image: " << status->text().toStdString()
                << '\n';
      return false;
    }
    timeline->setValue(timeline->maximum());
    QMetaObject::invokeMethod(timeline, "sliderReleased", Qt::DirectConnection);
    if (!wait_until([&] { return status->text().contains("No matching frame was presented"); }, 20000) ||
        play->text() == "Pause") {
      std::cerr << "A seek to a missing telemetry sample did not fail visibly within its deadline\n";
      return false;
    }
    dialog.reject();
    std::cout
        << "PASS: unequal-cadence repeat and end pause use actual decoded images; missing-sample seek fails bounded\n";
    return true;
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
  if (handshake.enabled() && requested_overlay_frame >= 0) {
    if (requested_overlay_frame > timeline->maximum())
      throw std::runtime_error("Paused geometry frame is outside the prepared range");
    timeline->setValue(requested_overlay_frame);
    QMetaObject::invokeMethod(timeline, "sliderReleased", Qt::DirectConnection);
    if (!wait_until([&] { return confirmed_camera_frame(dialog) == requested_overlay_frame; }))
      throw std::runtime_error("Paused geometry did not confirm the fixed overlay frame");
  }
  // Inspect the GPU front buffer: QWidget::grab omits native video, and X11
  // screen grabs can be black when this display is locked or composited.
  const auto visible_preview = [&](const char* phase) {
    QElapsedTimer settle;
    settle.start();
    const auto has_visible_video = [&](const QString& name) {
      const QImage screenshot_image(QDir(artifacts).filePath(name));
      const double scale = double(screenshot_image.width()) / dialog.width();
      const QPoint origin = target->mapTo(&dialog, QPoint(0, 0));
      const QImage image = screenshot_image.copy(QRect(
          qRound(origin.x() * scale),
          qRound(origin.y() * scale),
          qRound(target->width() * scale),
          qRound(target->height() * scale)));
      int bright = 0, samples = 0;
      for (int y = 0; y < image.height(); y += 8)
        for (int x = 0; x < image.width(); x += 8) {
          bright += qGray(image.pixel(x, y)) > 32;
          ++samples;
        }
      return samples > 0 && bright > samples / 10;
    };
    if (handshake.enabled()) {
      // Every synchronous readback in handoff mode has a pre-capture marker.
      // A black captured frame fails; do not hide it behind unobserved retries.
      const QString name = QString::fromLatin1(phase) + ".png";
      return wait_until([&] { return settle.elapsed() >= 200 && screenshot->isEnabled(); }, 5000) &&
          capture(name.toUtf8().constData()) && has_visible_video(name);
    }
    int attempt = 0;
    if (wait_until(
            [&] {
              if (settle.elapsed() < 200 || !screenshot->isEnabled())
                return false;
              const QString probe = QString("%1-probe-%2.png").arg(phase).arg(++attempt);
              if (!capture(probe.toUtf8().constData(), false))
                return false;
              return has_visible_video(probe);
            },
            5000))
      return capture((QString::fromLatin1(phase) + ".png").toUtf8().constData());
    std::cerr << "Native preview lost its image after " << phase << '\n';
    capture("failed-dialog.png", false);
    return false;
  };
  if (!visible_preview("paused-normal"))
    return false;
  const int paused_frame = timeline->value();
  const QSize normal_size = dialog.size();
  for (int i = 0; i < 2; ++i) {
    dialog.resize(i == 0 ? normal_size + QSize(100, 80) : normal_size);
    if (!visible_preview(i == 0 ? "paused-resized" : "paused-resize-restored") || timeline->value() != paused_frame)
      return false;
  }
  auto* focus = widget<QToolButton>(dialog, "maximizeCameraExperimentPreviewButton");
  for (int i = 0; i < 2; ++i) {
    focus->click();
    if (!visible_preview(i == 0 ? "paused-preview-maximized" : "paused-preview-restored") ||
        timeline->value() != paused_frame)
      return false;
  }
  auto* maximize = widget<QToolButton>(dialog, "maximizeCameraExperimentWindowButton");
  for (int i = 0; i < 2; ++i) {
    maximize->click();
    if (!visible_preview(i == 0 ? "paused-window-maximized" : "paused-window-restored") ||
        timeline->value() != paused_frame)
      return false;
  }
  int plot_seek_sequence = 0;
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
    return visible_preview(camera_capture_test::seek_phase(frame, ++plot_seek_sequence).toUtf8().constData());
  };
  if (plot->accessibleName() != "Visual timeline")
    return false;
  const int comparison_frame = timeline->maximum() / 2;
  const int overlay_frame = requested_overlay_frame >= 0 ? requested_overlay_frame : comparison_frame;
  if ((expected_frame_count >= 0 && timeline->maximum() != expected_frame_count - 1) ||
      overlay_frame > timeline->maximum()) {
    std::cerr << "Requested test range does not match prepared frame count " << timeline->maximum() + 1 << '\n';
    return false;
  }
  for (const int frame : exact_seek_frames) {
    if (frame > timeline->maximum()) {
      std::cerr << "Requested exact seek frame outside prepared range: " << frame << '\n';
      return false;
    }
  }
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
    const auto presented_frame = [&] {
      // The slider was set by this test. Require the separately published
      // native preview frame label as well as completed screenshot readiness.
      const QString expected = QString("frame %1 / %2").arg(frame + 1).arg(timeline->maximum() + 1);
      return screenshot->isEnabled() && timeline->value() == frame &&
          widget<QLabel>(dialog, "experimentFrameStatus")->text().split("  ·  ").value(2) == expected;
    };
    return wait_until([&]() { return presented_frame() || status->styleSheet().contains("#b42318"); }, 60000) &&
        presented_frame();
  };
  for (const int frame : exact_seek_frames) {
    if (!seek_frame(frame) || play->text() == "Pause") {
      std::cerr << "Exact seek failed at frame " << frame << ": " << status->text().toStdString() << '\n';
      return false;
    }
    std::cout << "Exact seek acknowledged frame " << frame << " / " << timeline->maximum() + 1 << ": "
              << widget<QLabel>(dialog, "experimentFrameStatus")->text().toStdString() << '\n';
    if (!capture(QString("exact-seek-%1.png").arg(frame).toUtf8().constData()))
      return false;
  }
  // Dense ranges cannot identify every frame through plot pixel coordinates.
  // Use the existing exact slider command for the opt-in overlay frame.
  if (!seek_frame(overlay_frame)) {
    std::cerr << "Overlay comparison seek failed: " << status->text().toStdString() << '\n';
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
    if (!capture((QString::fromLatin1(name) + ".png").toUtf8().constData()))
      return false;
    QElapsedTimer comparison_time;
    comparison_time.start();
    const QImage drawn = QImage(path).copy(video_rect);
    int different = 0;
    for (int y = 0; y < clean.height(); ++y)
      for (int x = 0; x < clean.width(); ++x)
        different += clean.pixel(x, y) != drawn.pixel(x, y);
    std::cout << "Test overlay pixel comparison " << name << " took " << comparison_time.elapsed() << " ms\n";
    if (different < 50 || timeline->value() != overlay_frame) {
      std::cerr << "Paused overlay did not draw on the same GPU frame: " << name << " changed=" << different << '\n';
      return false;
    }
    std::cout << name << " changed " << different << " preview pixels on paused frame " << overlay_frame << '\n';
    toggle->setChecked(false);
    if (!wait_until([&] { return screenshot->isEnabled(); }))
      return false;
  }
  // The plot paints on the CPU, so its layers can be checked directly: flipping
  // one must change what the plot draws on the same paused frame.
  if (handshake.enabled()) {
    std::vector<std::pair<QCheckBox*, bool>> previous;
    for (auto* toggle : dialog.findChildren<QCheckBox*>())
      if (toggle->objectName().startsWith("experimentPlotLayer_")) {
        previous.emplace_back(toggle, toggle->isChecked());
        toggle->setChecked(true);
      }
    if (!capture("plot-all-layers.png"))
      return false;
    for (const auto& [toggle, checked] : previous)
      toggle->setChecked(false);
    if (!capture("plot-no-layers.png"))
      return false;
    for (const auto& [toggle, checked] : previous)
      toggle->setChecked(checked);
  }
  for (const char* name :
       {"experimentPlotLayer_Recorded",
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
    if (handshake.enabled()) {
      const QString phase = "plot-layer-" + QString::fromLatin1(name).section('_', -1).toLower() +
          (toggle->isChecked() ? "-on.png" : "-off.png");
      if (!capture(phase.toUtf8().constData()))
        return false;
    }
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
  if (!seek_frame(overlay_frame) || !capture("experiment-original.png"))
    return false;
  // Require actual native presented-frame progress after Play, followed by an
  // end-to-start transition and several fresh wrapped frames. A Play bug that
  // restarts at zero must not satisfy the automatic-repeat check.
  const auto acknowledged_frame = [&] {
    const QString label = widget<QLabel>(dialog, "experimentFrameStatus")->text().split("  ·  ").value(2);
    const QStringList parts = label.mid(6).split(" / ");
    bool frame_ok = false, count_ok = false;
    const int frame = parts.value(0).toInt(&frame_ok) - 1;
    const int count = parts.value(1).toInt(&count_ok);
    return label.startsWith("frame ") && frame_ok && count_ok && count == timeline->maximum() + 1 && frame >= 0 &&
            frame < count && screenshot->isEnabled()
        ? frame
        : -1;
  };
  const double duration = args[6].toDouble();
  if (!std::isfinite(duration) || duration <= 0)
    return false;
  const double rate = (timeline->maximum() + 1) / duration;
  const int tail_span = std::max(3, std::min(timeline->maximum() / 3, int(std::ceil(rate * 3))));
  const int tail_start = timeline->maximum() - tail_span;
  const int near_end = timeline->maximum() - std::max(2, int(std::ceil(rate / 4)));
  const int wrapped_limit = std::min(timeline->maximum() / 3, int(std::ceil(rate * 2)));
  comparison->setCurrentIndex(2);
  if (!seek_frame(tail_start))
    return false;
  int prior_frame = tail_start, tail_frames = 0, wrapped_frames = 0;
  bool wrapped = false, invalid_repeat = false;
  play->click();
  const bool repeated = wait_until(
      [&] {
        if (status->styleSheet().contains("#b42318"))
          return true;
        const int frame = acknowledged_frame();
        if (frame < 0 || frame == prior_frame || play->text() != "Pause")
          return false;
        std::cout << "Repeat presented frame " << frame << " / " << timeline->maximum() + 1 << '\n';
        if (!wrapped) {
          if (frame > prior_frame && frame > tail_start)
            ++tail_frames;
          else if (frame < prior_frame && tail_frames >= 3 && prior_frame >= near_end && frame <= wrapped_limit) {
            wrapped = true;
            wrapped_frames = 1;
          } else
            invalid_repeat = true;
        } else if (frame > prior_frame && frame <= wrapped_limit)
          ++wrapped_frames;
        else
          invalid_repeat = true;
        prior_frame = frame;
        return invalid_repeat || wrapped_frames >= 3;
      },
      60000);
  if (!repeated || invalid_repeat || wrapped_frames < 3 || status->styleSheet().contains("#b42318")) {
    std::cerr << "Range repeat did not present a verified tail-to-wrap sequence: tail=" << tail_frames
              << " wrapped=" << wrapped_frames << ": " << status->text().toStdString() << '\n';
    return false;
  }
  play->click();
  if (!wait_until([&] { return play->text() != "Pause" && acknowledged_frame() >= 0; }, 60000) ||
      !capture("loop-held-frame.png")) {
    std::cerr << "Repeat Pause did not retain an acknowledged native frame\n";
    return false;
  }
  widget<QCheckBox>(dialog, "experimentLoop")->setChecked(false);
  if (!seek_frame(timeline->maximum() - 6))
    return false;
  play->click();
  if (!wait_until(
          [&]() {
            return (acknowledged_frame() == timeline->maximum() && play->text() != "Pause" &&
                    screenshot->isEnabled()) ||
                status->styleSheet().contains("#b42318");
          },
          60000) ||
      acknowledged_frame() != timeline->maximum() || !screenshot->isEnabled()) {
    std::cerr << "End-of-range pause failed: " << status->text().toStdString() << '\n';
    return false;
  }
  auto* save = widget<QPushButton>(dialog, "experimentSave");
  if (handshake.enabled() && !capture("end-paused.png"))
    return false;
  if (!save->isEnabled()) {
    std::cerr << "Prepared candidate cannot be saved\n";
    return false;
  }
  const QString saved_path = QDir(artifacts).absoluteFilePath("saved-camera-trial.yaml");
  if (QFileInfo::exists(saved_path) || !save_trial_dialog(save, saved_path, false) || QFileInfo::exists(saved_path) ||
      !save->isEnabled()) {
    std::cerr << "Cancelling Save changed the prepared trial or created a file\n";
    return false;
  }
  const QString cancelled_path = QDir(artifacts).absoluteFilePath("cancelled-camera-trial.yaml");
  auto* cancel_save = widget<QPushButton>(dialog, "experimentCancel");
  if (!save_trial_dialog(save, cancelled_path, true) || save->isEnabled() || !cancel_save->isEnabled() ||
      cancel_save->text() != "Cancel saving") {
    std::cerr << "An active Save must leave cancellation available\n";
    return false;
  }
  cancel_save->click();
  if (!wait_until([&] { return save->isEnabled(); }) || QFileInfo::exists(cancelled_path) ||
      !status->text().contains("Saving cancelled") || comparison->currentText() != "A · slower pan") {
    std::cerr << "Cancelling an active Save lost the prepared trial: " << status->text().toStdString() << '\n';
    return false;
  }
  record_heartbeat();
  QElapsedTimer save_time;
  save_time.start();
  const bool saved = save_trial_dialog(save, saved_path, true);
  record_heartbeat();
  std::cout << "Trial Save action took " << save_time.elapsed() << " ms\n";
  if (!saved || !wait_until([&] { return save->isEnabled() || status->styleSheet().contains("#b42318"); }) ||
      !QFileInfo::exists(saved_path) || status->styleSheet().contains("#b42318") || !status->text().contains("saved")) {
    std::cerr << "Save trial failed: " << status->text().toStdString() << '\n';
    return false;
  }
  // Parsing the exported evidence is test work; the actual Save above remains
  // inside the UI responsiveness measurement.
  const YAML::Node exported = YAML::LoadFile(saved_path.toStdString());
  const auto tuning = exported["tuning"]["play-tracker"]["hstream-runtime-tuning"];
  if (exported["schema"].as<std::string>() != "hstream-camera-experiment-v1" ||
      exported["name"].as<std::string>() != "A · slower pan" ||
      exported["frames"].size() != static_cast<std::size_t>(timeline->maximum() + 1) ||
      tuning["max-speed-x"].as<double>() != 1.5 || std::abs(tuning["max-accel-x"].as<double>() - 0.2) > 1e-7 ||
      tuning.size() != 2 || exported["tuning"]["play-tracker"]["hstream-apply-to-fast-box"].as<bool>() ||
      !exported["tuning"]["play-tracker"]["hstream-apply-to-follower-box"].as<bool>() ||
      exported["media"]["kind"].as<std::string>() != (QFileInfo(args[3]).isDir() ? "original-cameras" : "panorama") ||
      exported["frames"][0]["pts_ns"].as<uint64_t>() != exported["in_pts_ns"].as<uint64_t>() ||
      exported["frames"][exported["frames"].size() - 1]["pts_ns"].as<uint64_t>() >=
          exported["out_pts_ns"].as<uint64_t>()) {
    std::cerr << "Saved trial does not retain the selected native range and tuning\n";
    return false;
  }
  if (reference_trial && YAML::Dump(*reference_trial) != YAML::Dump(exported))
    throw std::runtime_error("Final trial Save differs from the immutable capture reference");
  heartbeat.restart();
  widget<QPushButton>(dialog, "experimentStopPreview")->click();
  if (!wait_until([&] { return play->isEnabled(); }) || !click_plot_frame(comparison_frame))
    return false;
  if (handshake.enabled() && !capture("reopened-preview.png"))
    return false;
  if (close_during_save) {
    const bool closed =
        close_save_probe(dialog, QDir(artifacts).absoluteFilePath("closed-camera-trial.yaml"), saved_path);
    std::cout << "PASS: separate Close/Save lifecycle probe; stale-source Save invalidation UNRUN in this option\n";
    return closed;
  }
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
      << "PASS: historical preparation, parameter trial, actual GPU playback, visible paused resize/maximize, visual timeline seek/resume, paused frame step, original comparison, range loop, end pause, screenshots, trial Save/Cancel, stale trial save invalidation\n";
  std::cout << "Longest UI heartbeat gap: " << longest_gap << " ms\n";
  if (longest_gap > 1000) {
    std::cerr << "Experiment blocked the UI event loop for more than one second\n";
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  // Qt's default 800px offscreen screen cannot fit the native 480px preview
  // beside the 390px controls pane. Give CPU geometry checks a real desktop
  // allocation; explicit caller platform configurations remain authoritative.
  QTemporaryDir offscreen;
  if (qgetenv("QT_QPA_PLATFORM") == "offscreen") {
    if (!offscreen.isValid())
      return 1;
    const QString configuration = offscreen.filePath("screen.json");
    camera_capture_test::write_json(
        configuration,
        {{"screens",
          QJsonArray{QJsonObject{
              {"name", "camera-test"},
              {"width", 1920},
              {"height", 1400},
              {"logicalDpi", 96},
              {"devicePixelRatio", 1}}}}});
    qputenv("QT_QPA_PLATFORM", "offscreen:configfile=" + QFile::encodeName(configuration));
  }
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication app(argc, argv);
  try {
    if (app.arguments().contains("--capture-driver-test"))
      return camera_capture_test::tests() ? 0 : 1;
    if (app.arguments().contains("--save-trial"))
      return save_trial_workflow(app.arguments()) ? 0 : 1;
    if (app.arguments().contains("--rink-comparison"))
      return rink_comparison(app.arguments()) ? 0 : 1;
    if (app.arguments().contains("--e2e"))
      return e2e(app.arguments()) ? 0 : 1;
    if (app.arguments().size() == 3 && app.arguments()[1] == "--confirmation")
      return confirmation(app.arguments()[2]) ? 0 : 1;
    return camera_capture_test::tests() && legacy_source_controls() && smoke() && screenshot_export_inventory() &&
            responsive_worker()
        ? 0
        : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
