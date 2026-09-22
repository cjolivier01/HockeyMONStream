#include "src/apps/hstream-ui/StitchingExperimentDialog.h"
#include "src/apps/hstream-ui/StitchingExperimentStore.h"

#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/PlayerFrameInputStore.h"
#include "hstream/src/libs/stitching/PlayerFrameSelection.h"

#include <opencv2/imgcodecs.hpp>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPixmap>
#include <QtGui/QScreen>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTableWidget>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

template <typename T>
T* widget(StitchingExperimentDialog& dialog, const char* name) {
  auto* result = dialog.findChild<T*>(name);
  if (!result)
    throw std::runtime_error(std::string("Missing widget: ") + name);
  return result;
}

void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

bool wait_until(const std::function<bool()>& ready, int timeout_ms) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < timeout_ms) {
    QCoreApplication::processEvents();
    if (ready())
      return true;
    QThread::msleep(10);
  }
  return false;
}

void add_options(StitchingExperimentDialog& dialog) {
  widget<QPushButton>(dialog, "addStitchExperimentsToBatchButton")->click();
  require(
      wait_until([&] { return widget<QLineEdit>(dialog, "stitchExperimentControlPoints")->isEnabled(); }, 30000),
      "Queued workspace persistence did not finish");
}

void discard_experiments(StitchingExperimentDialog& dialog) {
  QTimer::singleShot(0, &dialog, [&dialog]() {
    if (auto* prompt = dialog.findChild<QMessageBox*>("stitchExperimentDiscardGuard")) {
      if (auto* confirm = prompt->findChild<QPushButton*>("stitchExperimentDiscardConfirm"))
        confirm->click();
      else
        prompt->reject();
    }
  });
  widget<QPushButton>(dialog, "clearStitchExperimentBatchButton")->click();
  require(
      wait_until([&] { return widget<QLineEdit>(dialog, "stitchExperimentControlPoints")->isEnabled(); }, 30000),
      "Explicit experiment discard did not finish");
}

QByteArray read(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Cannot read fixture");
  return file.readAll();
}

void write(const QString& path, const QByteArray& contents) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly), "Cannot write fixture");
  require(file.write(contents) == contents.size(), "Incomplete fixture write");
}

void check_preview_layout(StitchingExperimentDialog& dialog) {
  auto* panel = widget<QWidget>(dialog, "stitchExperimentPreviewPanel");
  const std::vector<const char*> names = {
      "stitchExperimentVideo",
      "stitchExperimentPreviewStartLabel",
      "stitchExperimentPreviewStart",
      "stitchExperimentPreviewDurationLabel",
      "stitchExperimentPreviewDuration",
      "stitchExperimentLoop",
      "previewStitchExperimentButton",
      "stopStitchExperimentPreviewButton",
      "maximizeStitchExperimentButton"};
  std::vector<QRect> rectangles;
  for (const char* name : names) {
    auto* control = widget<QWidget>(dialog, name);
    const QRect rectangle(control->mapTo(panel, QPoint()), control->size());
    if (!control->isVisible() || !panel->rect().contains(rectangle) ||
        control->width() < control->minimumSizeHint().width() ||
        control->height() < control->minimumSizeHint().height())
      throw std::runtime_error(std::string("Clipped preview control: ") + name);
    for (const QRect& other : rectangles)
      if (rectangle.intersects(other))
        throw std::runtime_error(std::string("Overlapping preview control: ") + name);
    rectangles.push_back(rectangle);
  }
}

void exercise_layout(StitchingExperimentDialog& dialog) {
  require(dialog.windowFlags().testFlag(Qt::WindowMaximizeButtonHint), "Dialog must offer title-bar maximize");
  const QSize original_size = dialog.size();
  for (const QSize size : {QSize(1280, 820), QSize(1024, 720)}) {
    dialog.resize(size);
    QCoreApplication::processEvents();
    require(dialog.size() == size, "Dialog minimum size must fit a 1024x720 desktop");
    check_preview_layout(dialog);
  }
  auto* video = widget<QWidget>(dialog, "stitchExperimentVideo");
  auto* candidate_panel = widget<QWidget>(dialog, "stitchExperimentCandidatePanel");
  auto* splitter = widget<QSplitter>(dialog, "stitchExperimentSplitter");
  auto* expand = widget<QPushButton>(dialog, "maximizeStitchExperimentButton");
  const WId original_target = video->winId();
  const QSize normal_video_size = video->size();
  const QList<int> normal_splitter_sizes = splitter->sizes();
  const QString screenshot_dir = qEnvironmentVariable("HSTREAM_TEST_SCREENSHOT_DIR");
  if (!screenshot_dir.isEmpty())
    dialog.grab().save(screenshot_dir + "/stitch-layout-normal.png");
  expand->click();
  QCoreApplication::processEvents();
  require(!candidate_panel->isVisible() && expand->text() == "Restore layout", "Expand must focus the preview");
  require(video->width() > normal_video_size.width(), "Focused preview must gain horizontal space");
  require(video->winId() == original_target, "Expanding must not replace the GPU target");
  check_preview_layout(dialog);
  if (!screenshot_dir.isEmpty())
    dialog.grab().save(screenshot_dir + "/stitch-layout-expanded.png");
  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(&dialog, &escape);
  QCoreApplication::processEvents();
  require(dialog.isVisible() && candidate_panel->isVisible(), "Escape must restore layout without closing");
  require(splitter->sizes() == normal_splitter_sizes, "Restore must preserve the candidate/preview split");
  QMouseEvent double_click(
      QEvent::MouseButtonDblClick,
      QPointF(10, 10),
      QPointF(video->mapToGlobal(QPoint(10, 10))),
      Qt::LeftButton,
      Qt::LeftButton,
      Qt::NoModifier);
  QApplication::sendEvent(video, &double_click);
  QCoreApplication::processEvents();
  require(!candidate_panel->isVisible(), "Double-click must expand the preview");
  expand->click();
  QCoreApplication::processEvents();
  require(candidate_panel->isVisible() && video->winId() == original_target, "Button must restore the same target");
  dialog.showMaximized();
  QCoreApplication::processEvents();
  require(dialog.isMaximized(), "Dialog must maximize");
  check_preview_layout(dialog);
  dialog.showNormal();
  dialog.resize(original_size);
  QCoreApplication::processEvents();
}

void exercise_player_queue(const QString& game, const QString& root) {
  const QString runner = root + "/record-runner.sh";
  const QString arguments = root + "/runner-arguments.txt";
  write(
      runner,
      "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$HSTREAM_TEST_ARGUMENTS\"\nprintf '%s\\n' 'FAILED_PRECONDITION: Player overlap mapping canvas mismatch'\nexit 13\n");
  require(
      QFile::setPermissions(runner, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
      "Cannot make test runner executable");
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.insert("HM_OUTPUT_WORK_DIR", root + "/queue-output");
  environment.insert("HSTREAM_TEST_ARGUMENTS", arguments);
  StitchingExperimentDialog dialog(game, runner, root, root + "/config.yaml", environment, 100, 4, "00:00:00");
  dialog.show();
  QCoreApplication::processEvents();
  auto* players = widget<QCheckBox>(dialog, "stitchExperimentPreferPlayerFrames");
  auto* duration = widget<QSpinBox>(dialog, "stitchExperimentPlayerScanDuration");
  auto* remove = widget<QPushButton>(dialog, "removeStitchExperimentFromBatchButton");
  auto* table = widget<QTableWidget>(dialog, "stitchExperimentCandidates");
  auto* status = widget<QLabel>(dialog, "stitchExperimentStatus");
  require(
      !players->isChecked() && !duration->isEnabled() && duration->value() == 60 && duration->maximum() == 300,
      "Player selection must be opt-in with bounded search controls");
  add_options(dialog);
  players->setChecked(true);
  add_options(dialog);
  add_options(dialog);
  require(
      table->rowCount() == 2 && table->item(0, 0)->text().startsWith("Baseline") &&
          table->item(1, 0)->text().startsWith("Players"),
      "Automatic selection must reuse and upgrade an already queued ordinary baseline");
  duration->setValue(120);
  add_options(dialog);
  require(table->rowCount() == 2, "Changing search duration must retain the original immutable selection request");
  widget<QLineEdit>(dialog, "stitchExperimentControlPoints")->setText("150");
  add_options(dialog);
  require(
      table->rowCount() == 3 && table->item(2, 0)->text().startsWith("Players"),
      "A control-point variant must reuse the group's selection without another baseline");
  players->setChecked(false);
  widget<QLineEdit>(dialog, "stitchExperimentControlPoints")->setText("200");
  widget<QCheckBox>(dialog, "stitchExperimentSharedRotation")->setChecked(false);
  widget<QLineEdit>(dialog, "stitchExperimentRotations")->setText("-1/2");
  add_options(dialog);
  require(
      table->rowCount() == 4 && table->item(3, 0)->text().startsWith("Players"),
      "Unchecked variants must retain the group's selected frames while changing solve geometry");
  widget<QLineEdit>(dialog, "stitchExperimentStartFrames")->setText("00:00:01");
  add_options(dialog);
  require(
      table->rowCount() == 4 && status->text().contains("same frame count"),
      "One frame count must not silently create another reference-time selection");
  widget<QLineEdit>(dialog, "stitchExperimentStartFrames")->setText("00:00:00");
  table->selectRow(1);
  remove->click();
  require(table->rowCount() == 1, "Removing the selection owner must remove every reuse dependent");
  players->setChecked(true);
  widget<QCheckBox>(dialog, "stitchExperimentSharedRotation")->setChecked(true);
  widget<QLineEdit>(dialog, "stitchExperimentControlPoints")->setText("100");
  add_options(dialog);
  table->selectRow(0);
  remove->click();
  require(table->rowCount() == 0, "Removing a baseline must remove all dependent candidates");
  widget<QLineEdit>(dialog, "stitchExperimentFrameCounts")->setText("1,4");
  add_options(dialog);
  require(table->rowCount() == 3, "A one-frame candidate must not add a redundant scan or baseline");
  table->selectRow(2);
  remove->click();
  require(
      table->rowCount() == 2 && table->item(1, 0)->text().startsWith("Candidate"),
      "Removing the last automatic dependency must leave an ordinary baseline");
  discard_experiments(dialog);
  widget<QLineEdit>(dialog, "stitchExperimentControlPoints")->setText("100,150,200,250");
  widget<QLineEdit>(dialog, "stitchExperimentFrameCounts")->setText("1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16");
  add_options(dialog);
  require(
      table->rowCount() == 0 && status->text().contains("64-candidate"),
      "The queue limit must count baselines and automatic rows without partially adding them");
  widget<QLineEdit>(dialog, "stitchExperimentControlPoints")->setText("100");
  widget<QLineEdit>(dialog, "stitchExperimentFrameCounts")->setText("2");
  add_options(dialog);
  widget<QPushButton>(dialog, "startStitchExperimentBatchButton")->click();
  require(
      wait_until([&] { return status->text().startsWith("Batch complete."); }, 10000),
      "Failed automatic bootstrap did not finish its dependent queue");
  const QByteArray args = read(arguments);
  require(
      args.contains("--stitching-calibration-only\n") && args.contains("--stitching-calibration-with-ice-mask\n") &&
          !args.contains("--stitching-player-scan-output"),
      "Associated baseline must prepare its rink mask before any player scan");
  require(
      table->item(0, 5)->text().contains("Player overlap mapping canvas mismatch") &&
          table->item(0, 5)->toolTip().contains("Runner exited 13") &&
          table->item(1, 5)->text().contains("baseline calibration failed"),
      "A failed bootstrap must prevent its dependent scan from launching");
  dialog.reject();
  require(wait_until([&] { return !dialog.isVisible(); }, 1000), "Player queue dialog did not close");
}

void answer_close_guard(StitchingExperimentDialog& dialog, const char* choice, bool expect_use_enabled) {
  bool saw_guard = false;
  bool correct_state = false;
  QTimer::singleShot(0, &dialog, [&] {
    auto* prompt = dialog.findChild<QMessageBox*>("stitchExperimentCloseGuard");
    if (!prompt)
      return;
    saw_guard = true;
    auto* cancel = prompt->findChild<QPushButton*>("stitchExperimentCloseCancel");
    auto* use = prompt->findChild<QPushButton*>("stitchExperimentCloseUse");
    correct_state = cancel && use && prompt->defaultButton() == cancel && use->isEnabled() == expect_use_enabled;
    if (auto* button = prompt->findChild<QPushButton*>(choice))
      button->click();
    else
      prompt->reject();
  });
  dialog.close();
  require(saw_guard && correct_state, "Close guard must default to Cancel and enforce selected-candidate readiness");
}

void exercise_queued_reopen(const QString& game, const QString& root) {
  const auto environment = QProcessEnvironment::systemEnvironment();
  {
    StitchingExperimentDialog queued(
        game, root + "/record-runner.sh", root, root + "/config.yaml", environment, 100, 2, "00:00:00");
    queued.show();
    widget<QLineEdit>(queued, "stitchExperimentControlPoints")->setText("100,150");
    widget<QCheckBox>(queued, "stitchExperimentPreferPlayerFrames")->setChecked(true);
    add_options(queued);
    require(
        widget<QTableWidget>(queued, "stitchExperimentCandidates")->rowCount() == 3,
        "Queued persistence fixture must contain one baseline and two Players rows");
    queued.reject();
    require(wait_until([&] { return !queued.isVisible(); }, 1000), "Queued history did not close");
  }
  {
    StitchingExperimentDialog reopened(
        game, root + "/record-runner.sh", root, root + "/config.yaml", environment, 200, 2, "00:00:00");
    reopened.show();
    auto* table = widget<QTableWidget>(reopened, "stitchExperimentCandidates");
    require(
        table->rowCount() == 3 && table->item(0, 5)->text() == "Queued" && table->item(2, 5)->text() == "Queued",
        "Unstarted rows and settings must survive closing the dialog");
    add_options(reopened);
    require(
        table->rowCount() == 4 && table->item(3, 0)->text().startsWith("Players"),
        "An unchecked option must share the restored queue's count owner across sessions");
    reopened.reject();
    require(wait_until([&] { return !reopened.isVisible(); }, 1000), "Extended queued history did not close");
  }
  {
    auto recording_environment = environment;
    recording_environment.insert("HSTREAM_TEST_ARGUMENTS", root + "/queued-reopen-arguments.txt");
    StitchingExperimentDialog restored(
        game, root + "/record-runner.sh", root, root + "/config.yaml", recording_environment, 200, 2, "00:00:00");
    restored.show();
    auto* table = widget<QTableWidget>(restored, "stitchExperimentCandidates");
    require(table->rowCount() == 4, "Every queued row must reappear after another reopen");
    widget<QPushButton>(restored, "startStitchExperimentBatchButton")->click();
    require(
        wait_until(
            [&] { return widget<QLabel>(restored, "stitchExperimentStatus")->text().startsWith("Batch complete."); },
            10000),
        "Restored queued dependencies did not complete their failed bootstrap");
    require(
        table->item(0, 5)->text().contains("Player overlap mapping canvas mismatch") &&
            table->item(1, 5)->text().contains("baseline calibration failed") &&
            table->item(3, 5)->text().contains("shared frame selection"),
        "Restored cross-session dependencies must fail closed when their baseline fails");
    restored.reject();
    require(wait_until([&] { return !restored.isVisible(); }, 1000), "Completed restored queue did not close");
  }
}

void exercise_preparation_failure(const QString& game, const QString& root) {
  auto environment = QProcessEnvironment::systemEnvironment();
  const QString arguments = root + "/partial-preparation-arguments.txt";
  environment.insert("HSTREAM_TEST_ARGUMENTS", arguments);
  require(QFile::rename(game + "/right.mp4", game + "/right.offline"), "Cannot interrupt baseline source preparation");
  {
    StitchingExperimentDialog dialog(
        game, root + "/record-runner.sh", root, root + "/config.yaml", environment, 100, 2, "00:00:00");
    dialog.show();
    widget<QCheckBox>(dialog, "stitchExperimentPreferPlayerFrames")->setChecked(true);
    widget<QLineEdit>(dialog, "stitchExperimentControlPoints")->setText("100,150");
    widget<QPushButton>(dialog, "addStitchExperimentsToBatchButton")->click();
    require(
        wait_until(
            [&] {
              return widget<QLabel>(dialog, "stitchExperimentStatus")
                  ->text()
                  .startsWith("Could not persist the queued candidates:");
            },
            30000),
        "Baseline preparation failure did not finish");
    auto* table = widget<QTableWidget>(dialog, "stitchExperimentCandidates");
    require(
        table->rowCount() == 3 && table->item(1, 5)->text().contains("earlier queued workspace") &&
            table->item(2, 5)->text().contains("earlier queued workspace"),
        "A failed baseline preparation must stop both its Players owner and dependent solve");
    require(
        !widget<QPushButton>(dialog, "startStitchExperimentBatchButton")->isEnabled(),
        "A partially prepared queue cannot start");
    dialog.reject();
    require(wait_until([&] { return !dialog.isVisible(); }, 1000), "Failed preparation dialog did not close");
  }
  require(QFile::rename(game + "/right.offline", game + "/right.mp4"), "Cannot restore failed baseline source");
  StitchingExperimentDialog reopened(
      game, root + "/record-runner.sh", root, root + "/config.yaml", environment, 100, 2, "00:00:00");
  reopened.show();
  require(
      widget<QTableWidget>(reopened, "stitchExperimentCandidates")->rowCount() == 0 &&
          !widget<QPushButton>(reopened, "startStitchExperimentBatchButton")->isEnabled() && !QFile::exists(arguments),
      "Reopening partial preparation must never run orphan Players candidates as ordinary solves");
  reopened.reject();
}

void exercise_saved_selection(const QString& game, const QString& root) {
  using namespace hm::stitching;
  const QByteArray original = read(game + "/config.yaml");
  YAML::Node config = YAML::Load(original.toStdString());
  config["game"]["stitching"]["frame_offsets"]["left"] = 0;
  config["game"]["stitching"]["frame_offsets"]["right"] = 0;
  config["stitching"]["stitch_frame_time"] = "00:00:00.000";
  const auto context = player_frame_source_context(config, 0);
  require(context.ok(), "Saved-plan source context failed");
  PlayerFrameSelectionPlan plan;
  plan.settings.frame_count = 2;
  plan.context = {
      {"source_context", *context},
      {"baseline_generation", "fixture"},
      {"output_generation", "fixture"},
      {"detector_identity", "fixture"},
      {"rink_mask_sha256", "fixture"},
      {"rink_mask_revision", "fixture"},
      {"fieldmask_settings", "fixture"},
      {"output_rotation_degrees", "0"},
      {"decode_anchor_ns", "0"}};
  for (const char* camera : {"left.mp4", "right.mp4"}) {
    auto binding = BindPlayerFrameSource((game + "/" + camera).toStdString());
    require(binding.ok(), "Saved-plan source binding failed");
    plan.sources.push_back(*binding);
  }
  for (uint64_t i = 0; i < 2; ++i) {
    PlayerFrameObservation frame;
    frame.pair.timeline_pts_ns = i * kPlayerFrameSecond;
    for (size_t camera = 0; camera < 2; ++camera)
      frame.pair.cameras[camera] = {plan.sources[camera].path, i * kPlayerFrameSecond};
    frame.eligible_people = 1;
    frame.coverage = {5};
    frame.size_band_counts = {1, 0, 0};
    frame.quality = 1;
    plan.selected.push_back(frame);
  }
  auto fingerprint = PlayerFrameSelectionFingerprint(plan);
  require(fingerprint.ok(), "Saved-plan fingerprint failed");
  plan.fingerprint = *fingerprint;
  config["stitching"]["calibration_frame_selection"] = PlayerFrameSelectionPlanYaml(plan);
  std::vector<std::array<std::filesystem::path, 2>> inputs;
  for (size_t index = 0; index < plan.selected.size(); ++index) {
    std::array<std::filesystem::path, 2> pair;
    for (size_t camera = 0; camera < 2; ++camera) {
      pair[camera] = (game + QString("/input-%1-%2.png").arg(index).arg(camera)).toStdString();
      require(
          cv::imwrite(pair[camera].string(), cv::Mat(12, 16, CV_8UC3, cv::Scalar(index + 10, camera + 20, 30))),
          "Cannot create retained input fixture");
    }
    inputs.push_back(pair);
  }
  require(PublishPlayerFrameInputs(game.toStdString(), plan, inputs).ok(), "Cannot publish retained frame fixture");
  config["stitching"]["calibration_frame_inputs_fingerprint"] = plan.fingerprint;
  config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "fixture-main-calibration";
  config["hstream_ui"]["stitching_calibration"]["status"] = "complete";
  config["hstream_ui"]["stitching_calibration"]["control_points"] = 100;
  config["hstream_ui"]["stitching_calibration"]["frame_count"] = 2;
  write(game + "/config.yaml", QByteArray::fromStdString(YAML::Dump(config)));
  {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("HM_OUTPUT_WORK_DIR", root + "/saved-output");
    const QString arguments = root + "/saved-runner-arguments.txt";
    environment.insert("HSTREAM_TEST_ARGUMENTS", arguments);
    StitchingExperimentDialog dialog(
        game, root + "/record-runner.sh", root, root + "/config.yaml", environment, 100, 2, "00:00:00");
    dialog.show();
    auto* points = widget<QLineEdit>(dialog, "stitchExperimentControlPoints");
    auto* table = widget<QTableWidget>(dialog, "stitchExperimentCandidates");
    auto* status = widget<QLabel>(dialog, "stitchExperimentStatus");
    points->setText("100,150");
    add_options(dialog);
    require(
        table->rowCount() == 3 && table->item(0, 0)->text() == "Main calibration" &&
            table->item(1, 0)->text().startsWith("Players") && table->item(2, 0)->text().startsWith("Players"),
        "An unchecked saved selection must produce only solve candidates, without an ordinary baseline");
    widget<QLineEdit>(dialog, "stitchExperimentStartFrames")->setText("00:00:01");
    add_options(dialog);
    require(table->rowCount() == 3 && status->text().contains("reference"), "Saved reference conflicts must fail");
    widget<QLineEdit>(dialog, "stitchExperimentStartFrames")->setText("00:00:00");
    widget<QPushButton>(dialog, "startStitchExperimentBatchButton")->click();
    require(
        wait_until([&] { return status->text().startsWith("Batch complete."); }, 10000),
        "Saved-plan solve queue stalled");
    require(
        !read(arguments).contains("--force-reconfigure") && !read(arguments).contains("--stitching-player-scan-output"),
        "Saved frames must solve directly without orientation reset or another scan");
    require(
        widget<QPlainTextEdit>(dialog, "stitchExperimentLog")
            ->toPlainText()
            .contains(QString::fromStdString(*fingerprint)),
        "Reuse must report the exact selection fingerprint");
    answer_close_guard(dialog, "stitchExperimentCloseCancel", false);
    require(dialog.isVisible() && table->rowCount() == 3, "Cancel must keep unapplied frozen plans");
    answer_close_guard(dialog, "stitchExperimentCloseDiscard", false);
    require(
        wait_until([&] { return !dialog.isVisible(); }, 1000), "Keeping results must close a failed saved-plan batch");
  }
  config["stitching"]["control_point_matcher"] = "sift";
  config["stitching"]["projection_framing"]["horizontal_fov"] = 170;
  write(game + "/config.yaml", QByteArray::fromStdString(YAML::Dump(config)));
  {
    auto environment = QProcessEnvironment::systemEnvironment();
    const QString arguments = root + "/reopened-runner-arguments.txt";
    environment.insert("HSTREAM_TEST_ARGUMENTS", arguments);
    require(
        QFile::rename(game + "/left.mp4", game + "/left.offline") &&
            QFile::rename(game + "/right.mp4", game + "/right.offline"),
        "Cannot take fixture sources offline");
    StitchingExperimentDialog reopened(
        game, root + "/record-runner.sh", root, root + "/config.yaml", environment, 100, 2, "00:00:01");
    reopened.show();
    auto* table = widget<QTableWidget>(reopened, "stitchExperimentCandidates");
    require(
        table->rowCount() == 3 && table->item(0, 0)->text() == "Main calibration",
        "Reopening must restore the main calibration and every saved result");
    require(
        widget<QLabel>(reopened, "stitchExperimentCacheLocation")->text().contains(game + "/stitching-experiments"),
        "Experiment storage must remain inside its associated game");
    table->selectRow(1);
    bool inspected_log = false;
    QTimer::singleShot(0, &reopened, [&]() {
      if (auto* viewer = reopened.findChild<QDialog*>("stitchExperimentRunnerLogViewer")) {
        auto* contents = viewer->findChild<QPlainTextEdit*>("stitchExperimentRetainedRunnerLog");
        inspected_log = contents && contents->toPlainText().contains("Player overlap mapping canvas mismatch");
        viewer->accept();
      }
    });
    widget<QPushButton>(reopened, "viewStitchExperimentRunnerLogButton")->click();
    require(inspected_log, "Reopened historical rows must expose their retained runner output");
    table->selectRow(0);
    bool inspected_main = false;
    QTimer::singleShot(0, &reopened, [&]() {
      if (auto* inspector = reopened.findChild<QDialog*>("stitchExperimentFrameInspector")) {
        auto* frames = inspector->findChild<QTableWidget*>("stitchExperimentSelectedFrames");
        inspected_main = frames && frames->rowCount() == 2;
        inspector->accept();
      }
    });
    widget<QPushButton>(reopened, "inspectStitchExperimentFramesButton")->click();
    require(inspected_main, "Main frame inspector must show the retained selected pairs");
    require(!QFile::exists(arguments), "Main frame inspection must not launch baseline, scan or extraction");
    require(
        QFile::rename(game + "/left.offline", game + "/left.mp4") &&
            QFile::rename(game + "/right.offline", game + "/right.mp4"),
        "Cannot restore fixture sources");
    widget<QLineEdit>(reopened, "stitchExperimentStartFrames")->setText("00:00:00");
    add_options(reopened);
    require(table->rowCount() == 4, "A retained identical matrix must permit a new solve after configuration changes");
    add_options(reopened);
    require(table->rowCount() == 4, "Repeated Add with unchanged configuration must still suppress duplicates");
    const auto check_latest_inputs = [&](const PlayerFrameSelectionPlan& expected) {
      const auto opened = OpenStitchingExperimentStore(game.toStdString());
      require(opened.ok(), "Cannot open retained rerun catalog");
      const auto catalog = LoadStitchingExperimentStore(*opened);
      require(catalog.ok() && !catalog->experiments.empty(), "Cannot load retained rerun catalog");
      const auto& latest = catalog->experiments.back();
      const YAML::Node candidate_config = YAML::LoadFile((latest.workspace.game_directory / "config.yaml").string());
      require(
          latest.workspace.settings.control_points == 100 &&
              latest.saved_selection_fingerprint == expected.fingerprint &&
              candidate_config["stitching"]["control_point_matcher"].as<std::string>() == "sift" &&
              candidate_config["stitching"]["projection_framing"]["horizontal_fov"].as<int>() == 170,
          "Rerunning a saved matrix must use the current main solve settings and selection fingerprint");
      const auto retained = LoadPlayerFrameInputs(latest.workspace.game_directory, expected);
      require(retained.ok() && retained->has_value(), "The rerun must copy the exact frozen input bundle");
    };
    check_latest_inputs(plan);
    widget<QPushButton>(reopened, "startStitchExperimentBatchButton")->click();
    require(
        wait_until(
            [&] { return widget<QLabel>(reopened, "stitchExperimentStatus")->text().startsWith("Batch complete."); },
            10000),
        "Reopened retained-frame solve did not finish");
    require(
        !read(arguments).contains("--force-reconfigure") && !read(arguments).contains("--stitching-player-scan-output"),
        "Reopened saved frames must solve without another baseline or scan");
    widget<QLineEdit>(reopened, "stitchExperimentControlPoints")->setText("150");
    add_options(reopened);
    require(table->rowCount() == 5, "Retain an unstarted solve using the previous main selection");
    widget<QLineEdit>(reopened, "stitchExperimentControlPoints")->setText("100");
    PlayerFrameSelectionPlan replacement = plan;
    replacement.selected.back().pair.timeline_pts_ns = 2 * kPlayerFrameSecond;
    for (auto& camera : replacement.selected.back().pair.cameras)
      camera.source_pts_ns = 2 * kPlayerFrameSecond;
    const auto replacement_fingerprint = PlayerFrameSelectionFingerprint(replacement);
    require(replacement_fingerprint.ok(), "Cannot fingerprint replacement selection");
    replacement.fingerprint = *replacement_fingerprint;
    require(
        PublishPlayerFrameInputs(game.toStdString(), replacement, inputs).ok(), "Cannot publish replacement selection");
    config["stitching"]["calibration_frame_selection"] = PlayerFrameSelectionPlanYaml(replacement);
    config["stitching"]["calibration_frame_inputs_fingerprint"] = replacement.fingerprint;
    write(game + "/config.yaml", QByteArray::fromStdString(YAML::Dump(config)));
    add_options(reopened);
    require(
        table->rowCount() == 6, "A replacement main fingerprint must permit a new identical solve in the same dialog");
    check_latest_inputs(replacement);
    add_options(reopened);
    require(table->rowCount() == 6, "Repeated Add of the replacement selection must still suppress duplicates");
    widget<QPushButton>(reopened, "startStitchExperimentBatchButton")->click();
    require(
        wait_until(
            [&] { return widget<QLabel>(reopened, "stitchExperimentStatus")->text().startsWith("Batch complete."); },
            10000),
        "Queued solves must complete after main replaces their frozen selection");
    require(
        table->item(4, 5)->text().contains("Player overlap mapping canvas mismatch") &&
            table->item(5, 5)->text().contains("Player overlap mapping canvas mismatch"),
        "Both old and replacement solves must retain their actual runner result without a persistence error");
    const auto completed_store = OpenStitchingExperimentStore(game.toStdString());
    require(completed_store.ok(), "Cannot reopen completed selection history");
    const auto completed = LoadStitchingExperimentStore(*completed_store);
    require(
        completed.ok() && completed->experiments.size() == 5 &&
            completed->experiments[3].selection_fingerprint == plan.fingerprint &&
            completed->experiments[4].selection_fingerprint == replacement.fingerprint &&
            completed->selected_by_count.at(2) ==
                completed->experiments[4]
                    .workspace.game_directory.lexically_relative(completed_store->directory)
                    .generic_string(),
        "Late old-plan completion must preserve both histories and the current main count authority");
    discard_experiments(reopened);
    require(
        table->rowCount() == 1 && table->item(0, 0)->text() == "Main calibration" &&
            LoadPlayerFrameInputs(game.toStdString(), plan).ok(),
        "Explicit history discard must preserve the main calibration's immutable frame bundle");
    reopened.reject();
    require(wait_until([&] { return !reopened.isVisible(); }, 1000), "Main-only reopened dialog did not close");
  }
  // A main-derived queued row must retain its count even if main switches to a
  // different count before any solve publishes selected_by_count for this one.
  config["stitching"]["calibration_frame_selection"] = PlayerFrameSelectionPlanYaml(plan);
  config["stitching"]["calibration_frame_inputs_fingerprint"] = plan.fingerprint;
  write(game + "/config.yaml", QByteArray::fromStdString(YAML::Dump(config)));
  PlayerFrameSelectionPlan three = plan;
  three.settings.frame_count = 3;
  auto third = three.selected.back();
  third.pair.timeline_pts_ns = 2 * kPlayerFrameSecond;
  for (auto& camera : third.pair.cameras)
    camera.source_pts_ns = 2 * kPlayerFrameSecond;
  three.selected.push_back(third);
  const auto three_fingerprint = PlayerFrameSelectionFingerprint(three);
  require(three_fingerprint.ok(), "Cannot fingerprint alternate main frame count");
  three.fingerprint = *three_fingerprint;
  auto three_inputs = inputs;
  three_inputs.push_back(inputs.back());
  require(
      PublishPlayerFrameInputs(game.toStdString(), three, three_inputs).ok(), "Cannot publish alternate main count");
  {
    StitchingExperimentDialog observer(
        game,
        root + "/record-runner.sh",
        root,
        root + "/config.yaml",
        QProcessEnvironment::systemEnvironment(),
        200,
        2,
        "00:00:00");
    observer.show();
    StitchingExperimentDialog queued(
        game,
        root + "/record-runner.sh",
        root,
        root + "/config.yaml",
        QProcessEnvironment::systemEnvironment(),
        100,
        2,
        "00:00:00");
    queued.show();
    add_options(queued);
    config["stitching"]["calibration_frame_selection"] = PlayerFrameSelectionPlanYaml(three);
    config["stitching"]["calibration_frame_inputs_fingerprint"] = three.fingerprint;
    config["hstream_ui"]["stitching_calibration"]["frame_count"] = 3;
    write(game + "/config.yaml", QByteArray::fromStdString(YAML::Dump(config)));
    widget<QLineEdit>(queued, "stitchExperimentControlPoints")->setText("150");
    add_options(queued);
    auto* table = widget<QTableWidget>(queued, "stitchExperimentCandidates");
    require(
        table->rowCount() == 3 && table->item(2, 0)->text().startsWith("Players"),
        "In-memory Add must reuse the queued main-derived count after main switches counts");
    add_options(observer);
    require(
        widget<QTableWidget>(observer, "stitchExperimentCandidates")->rowCount() == 1 &&
            widget<QLabel>(observer, "stitchExperimentStatus")
                ->text()
                .contains("Another dialog saved this frame count"),
        "A newly discovered count owner requires reload instead of falling through to an ordinary solve");
    observer.reject();
    answer_close_guard(queued, "stitchExperimentCloseDiscard", false);
    require(wait_until([&] { return !queued.isVisible(); }, 1000), "Retained-count queue did not close");
  }
  {
    auto environment = QProcessEnvironment::systemEnvironment();
    const QString arguments = root + "/retained-count-runner-arguments.txt";
    environment.insert("HSTREAM_TEST_ARGUMENTS", arguments);
    StitchingExperimentDialog reopened(
        game, root + "/record-runner.sh", root, root + "/config.yaml", environment, 200, 2, "00:00:00");
    reopened.show();
    auto* table = widget<QTableWidget>(reopened, "stitchExperimentCandidates");
    add_options(reopened);
    require(
        table->rowCount() == 4 && table->item(3, 0)->text().startsWith("Players"),
        "Reopening an unstarted frozen count must add only a solve, without a new baseline");
    widget<QPushButton>(reopened, "startStitchExperimentBatchButton")->click();
    require(
        wait_until(
            [&] { return widget<QLabel>(reopened, "stitchExperimentStatus")->text().startsWith("Batch complete."); },
            10000),
        "Retained count solve queue did not finish");
    require(
        table->item(3, 5)->text().contains("Player overlap mapping canvas mismatch") &&
            !read(arguments).contains("--force-reconfigure") &&
            !read(arguments).contains("--stitching-player-scan-output"),
        "Retained count variants must run directly using their frozen inputs");
    const auto opened = OpenStitchingExperimentStore(game.toStdString());
    require(opened.ok(), "Cannot open retained-count results");
    const auto catalog = LoadStitchingExperimentStore(*opened);
    require(catalog.ok() && catalog->experiments.size() == 3, "Every retained-count solve must remain durable");
    for (const auto& row : catalog->experiments) {
      const auto retained = LoadPlayerFrameInputs(row.workspace.game_directory, plan);
      require(
          row.selection_fingerprint == plan.fingerprint && retained.ok() && retained->has_value(),
          "Every retained-count solve must preserve the original exact pairs and full PNG bundle");
    }
    require(
        catalog->selected_by_count.count(2), "Completed retained count must remain indexed beside main's other count");
    discard_experiments(reopened);
    reopened.reject();
  }
  write(game + "/config.yaml", original);
}

void exercise_close_promotion_failure(const QString& game, const QString& root) {
  const QString fixtures = root + "/synthetic-artifacts";
  require(QDir().mkpath(fixtures), "Cannot create synthetic artifacts");
  for (const char* name :
       {"mapping_0000.tif",
        "mapping_0000_x.tif",
        "mapping_0000_y.tif",
        "mapping_0001.tif",
        "mapping_0001_x.tif",
        "mapping_0001_y.tif"})
    require(
        cv::imwrite((fixtures + "/" + name).toStdString(), cv::Mat(8, 8, CV_16UC1, cv::Scalar(1))),
        "Cannot write TIFF fixture");
  require(
      cv::imwrite((fixtures + "/seam_file.png").toStdString(), cv::Mat(8, 8, CV_8UC1, cv::Scalar(255))),
      "Cannot write seam fixture");
  write(fixtures + "/hm_project.pto", "invalid promotion geometry");
  write(fixtures + "/autooptimiser_out.pto", "invalid promotion geometry");
  const QString runner = root + "/complete-runner.sh";
  write(runner, "#!/bin/sh\ncp \"$HSTREAM_TEST_ARTIFACTS\"/* \"$HM_GAME_DIR/$2/\"\n");
  require(
      QFile::setPermissions(runner, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
      "Cannot prepare completion runner");
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("HM_OUTPUT_WORK_DIR", root + "/promotion-output");
  environment.insert("HSTREAM_TEST_ARTIFACTS", fixtures);
  StitchingExperimentDialog dialog(game, runner, root, root + "/config.yaml", environment, 100, 1, "00:00:00");
  dialog.show();
  add_options(dialog);
  widget<QPushButton>(dialog, "startStitchExperimentBatchButton")->click();
  auto* status = widget<QLabel>(dialog, "stitchExperimentStatus");
  require(
      wait_until([&] { return status->text().startsWith("Batch complete."); }, 10000), "Synthetic completion stalled");
  auto* table = widget<QTableWidget>(dialog, "stitchExperimentCandidates");
  require(table->item(0, 5)->text() == "Ready", "Synthetic candidate must complete before testing promotion failure");
  answer_close_guard(dialog, "stitchExperimentCloseUse", true);
  require(
      wait_until([&] { return widget<QPushButton>(dialog, "applyStitchExperimentButton")->isEnabled(); }, 10000),
      "Failed promotion did not restore controls");
  require(
      dialog.isVisible() &&
          widget<QPlainTextEdit>(dialog, "stitchExperimentLog")->toPlainText().contains("publication failed"),
      "Failed promotion must retain the dialog and its results");
  answer_close_guard(dialog, "stitchExperimentCloseDiscard", true);
  require(wait_until([&] { return !dialog.isVisible(); }, 1000), "Keeping results must close completed results");
}

void exercise_player_cancellation(const QString& game, const QString& root) {
  // A shell fixture remains running without interpreting the runner arguments.
  const QString runner = root + "/waiting-runner.sh";
  write(runner, "#!/bin/sh\nexec sleep 30\n");
  require(
      QFile::setPermissions(runner, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
      "Cannot make waiting runner executable");
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("HM_OUTPUT_WORK_DIR", root + "/cancellation-output");
  StitchingExperimentDialog waiting(game, runner, root, root + "/config.yaml", environment, 100, 2, "00:00:00");
  waiting.show();
  widget<QCheckBox>(waiting, "stitchExperimentPreferPlayerFrames")->setChecked(true);
  add_options(waiting);
  widget<QPushButton>(waiting, "startStitchExperimentBatchButton")->click();
  auto* cancel = widget<QPushButton>(waiting, "cancelStitchExperimentsButton");
  require(wait_until([&] { return cancel->isEnabled(); }, 1000), "Bootstrap never became cancellable");
  cancel->click();
  auto* status = widget<QLabel>(waiting, "stitchExperimentStatus");
  require(
      wait_until([&] { return status->text().startsWith("Batch cancelled."); }, 10000),
      "Cancellation did not stop the owned process session");
  auto* table = widget<QTableWidget>(waiting, "stitchExperimentCandidates");
  require(
      table->item(0, 5)->text() == "Cancelled" && table->item(1, 5)->text() == "Cancelled",
      "Cancelled bootstrap must never advance its automatic candidate");
  waiting.reject();
  require(wait_until([&] { return !waiting.isVisible(); }, 1000), "Cancelled player dialog did not close");
}

void exercise(
    const QString& game,
    const QString& runner,
    const QString& repo,
    bool gpu,
    const QString& log_path,
    bool player_selection = false) {
  const QString anchor = player_selection ? qEnvironmentVariable("HSTREAM_TEST_PLAYER_ANCHOR", "00:00:00") : "00:00:00";
  const QByteArray original_config = read(game + "/config.yaml");
  const QStringList original_files = QDir(game).entryList(QDir::Files);
  bool selected = false;
  QTemporaryDir experiment_cache;
  require(experiment_cache.isValid(), "Cannot create isolated experiment output for test");
  auto experiment_environment = QProcessEnvironment::systemEnvironment();
  experiment_environment.insert("HM_OUTPUT_WORK_DIR", experiment_cache.path());
  StitchingExperimentDialog dialog(
      game,
      runner,
      repo,
      repo + "/configs/ds_hockey_app_config.yaml",
      experiment_environment,
      100,
      1,
      anchor,
      nullptr,
      [&] { selected = true; });
  auto* log = widget<QPlainTextEdit>(dialog, "stitchExperimentLog");
  struct SaveLog {
    QPlainTextEdit* log;
    QString path;
    void save() const {
      if (!path.isEmpty()) {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly))
          file.write(log->toPlainText().toUtf8());
      }
    }
    ~SaveLog() {
      save();
    }
  } save_log{log, log_path};
  QTimer log_timer;
  QObject::connect(&log_timer, &QTimer::timeout, &dialog, [&] { save_log.save(); });
  if (!log_path.isEmpty())
    log_timer.start(2000);
  dialog.show();
  dialog.raise();
  QCoreApplication::processEvents();
  if (!gpu)
    exercise_layout(dialog);
  widget<QLineEdit>(dialog, "stitchExperimentControlPoints")->setText("100,150");
  widget<QLineEdit>(dialog, "stitchExperimentFrameCounts")->setText(player_selection ? "2" : "1");
  widget<QLineEdit>(dialog, "stitchExperimentStartFrames")->setText(anchor);
  if (player_selection) {
    widget<QCheckBox>(dialog, "stitchExperimentPreferPlayerFrames")->setChecked(true);
    widget<QSpinBox>(dialog, "stitchExperimentPlayerScanDuration")->setValue(10);
  }
  auto* start = widget<QPushButton>(dialog, "startStitchExperimentBatchButton");
  auto* table = widget<QTableWidget>(dialog, "stitchExperimentCandidates");
  const int initial_rows = table->rowCount();
  add_options(dialog);
  add_options(dialog);
  const int expected_rows = initial_rows + (player_selection ? 3 : 2);
  require(table->rowCount() == expected_rows && start->isEnabled(), "Queue must deduplicate without running");
  require(read(game + "/config.yaml") == original_config, "Queue changed source config");
  start->click();
  auto* status = widget<QLabel>(dialog, "stitchExperimentStatus");
  require(
      wait_until([&] { return status->text().startsWith("Batch complete."); }, gpu ? 900000 : 10000),
      "Batch did not finish");
  require(read(game + "/config.yaml") == original_config, "Calibration changed source config");
  if (gpu) {
    if (player_selection)
      require(
          log->toPlainText().count("HSTREAM_PLAYER_FRAME_SCAN observations=") == 1,
          "Control-point variants must perform exactly one player scan for their shared frame count");
    widget<QCheckBox>(dialog, "stitchExperimentLoop")->setChecked(false);
    widget<QSpinBox>(dialog, "stitchExperimentPreviewDuration")->setValue(5);
    auto* play = widget<QPushButton>(dialog, "previewStitchExperimentButton");
    for (int row = initial_rows; row < expected_rows; ++row) {
      if (table->item(row, 5)->text() != "Ready")
        throw std::runtime_error(
            QString("Candidate %1 failed: %2").arg(row + 1).arg(table->item(row, 5)->text()).toStdString());
      table->selectRow(row);
      QCoreApplication::processEvents();
      require(play->isEnabled(), "Ready candidate cannot preview");
      const int log_offset = log->toPlainText().size();
      QElapsedTimer playback;
      playback.start();
      play->click();
      bool captured = false;
      QElapsedTimer first_presented;
      require(
          wait_until(
              [&] {
                // One bounded display capture per passage is test evidence only; the
                // production preview never reads video surfaces back to the CPU.
                if (!captured && log->toPlainText().mid(log_offset).contains("message=first GPU frame presented") &&
                    QGuiApplication::primaryScreen()) {
                  if (!first_presented.isValid())
                    first_presented.start();
                  if (first_presented.elapsed() >= 200) {
                    auto* video = widget<QWidget>(dialog, "stitchExperimentVideo");
                    const QPoint position = video->mapToGlobal(QPoint(0, 0));
                    captured = QGuiApplication::primaryScreen()
                                   ->grabWindow(0, position.x(), position.y(), video->width(), video->height())
                                   .save(log_path + QString(".candidate-%1.png").arg(row + 1));
                  }
                }
                return play->isEnabled();
              },
              120000),
          "Preview did not finish");
      require(playback.elapsed() >= 4250, "Five-second preview was not clock paced");
      const QString passage_log = log->toPlainText().mid(log_offset);
      require(
          !passage_log.contains("HSTREAM_CALIBRATION stage=features status=started") &&
              !passage_log.contains("HSTREAM_CALIBRATION stage=canvas status=started") &&
              !passage_log.contains("Removed "),
          "Preview regenerated the selected calibration");
      const QRegularExpression fps("(?:\\*\\*PERF:\\s+|\\bfps=)([0-9]+(?:\\.[0-9]+)?)");
      auto matches = fps.globalMatch(passage_log);
      bool processed_frames = false;
      while (matches.hasNext())
        processed_frames |= matches.next().captured(1).toDouble() > 0;
      // Five-second passages can finish before the default five-second PERF
      // timer. The renderer's acknowledgement proves GPU presentation in that case.
      require(
          (processed_frames || passage_log.contains("message=first GPU frame presented")) &&
              passage_log.contains("App run successful"),
          "Preview did not present frames");
    }
    require(read(game + "/config.yaml") == original_config, "Preview changed source config");
    require(
        QDir(game).entryList(QDir::Files) == original_files, "Experiments published source artifacts before selection");
    if (player_selection) {
      auto* inspect = widget<QPushButton>(dialog, "inspectStitchExperimentFramesButton");
      QString first_player_identity;
      for (int row = initial_rows; row < expected_rows; ++row) {
        table->selectRow(row);
        require(inspect->isEnabled(), "Frame inspection must be available for the highlighted baseline or Players row");
        bool inspected = false;
        QString inspected_identity;
        QTimer::singleShot(0, &dialog, [&] {
          auto* viewer = dialog.findChild<QDialog*>("stitchExperimentFrameInspector");
          if (!viewer)
            return;
          auto* frames = viewer->findChild<QTableWidget*>("stitchExperimentSelectedFrames");
          auto* identity = viewer->findChild<QPlainTextEdit*>("stitchExperimentSelectedFrameIdentity");
          auto* left = viewer->findChild<QLabel*>("stitchExperimentSelectedLeft");
          auto* right = viewer->findChild<QLabel*>("stitchExperimentSelectedRight");
          if (frames && frames->rowCount() == 2)
            frames->selectRow(1);
          const auto thumbnail_has_tones = [](const QLabel* label) {
            if (!label)
              return false;
            const QPixmap pixmap = label->property("pixmap").value<QPixmap>();
            if (pixmap.isNull())
              return false;
            const QImage sample = pixmap.scaled(64, 36).toImage();
            size_t midtones = 0;
            for (int y = 0; y < sample.height(); ++y) {
              for (int x = 0; x < sample.width(); ++x) {
                const int gray = qGray(sample.pixel(x, y));
                midtones += gray >= 16 && gray <= 239;
              }
            }
            // This real hockey fixture has gray ice and players. A 16-bit still
            // written directly to JPEG clips almost every pixel to white/cyan.
            return midtones * 10 >= static_cast<size_t>(sample.width() * sample.height());
          };
          inspected_identity = identity ? identity->toPlainText() : QString();
          inspected = frames && frames->rowCount() == 2 && identity && thumbnail_has_tones(left) &&
              thumbnail_has_tones(right) &&
              (row == initial_rows ? inspected_identity.contains("Source time:") &&
                       !inspected_identity.contains("Selection fingerprint:")
                                   : inspected_identity.contains("Source PTS:") &&
                       inspected_identity.contains("Selection fingerprint:"));
          if (!log_path.isEmpty())
            viewer->grab().save(log_path + QString(".candidate-%1-frames.png").arg(row + 1));
          viewer->accept();
        });
        inspect->click();
        require(
            inspected, "Frame inspection must show exact identities and both thumbnails with preserved image tones");
        if (row == initial_rows + 1)
          first_player_identity = inspected_identity;
        else if (row > initial_rows + 1)
          require(
              inspected_identity == first_player_identity,
              "Both Players solves must use identical physical pairs and selection fingerprint");
      }
    }
    auto* apply = widget<QPushButton>(dialog, "applyStitchExperimentButton");
    require(apply->isEnabled(), "Ready candidate cannot be selected");
    answer_close_guard(dialog, "stitchExperimentCloseUse", true);
    require(wait_until([&] { return selected || apply->isEnabled(); }, 60000), "Candidate promotion timed out");
    if (!selected)
      throw std::runtime_error("Candidate promotion failed: " + status->text().toStdString());
    require(read(game + "/config.yaml") != original_config, "Explicit selection did not publish config");
    require(
        wait_until([&] { return !dialog.isVisible(); }, 1000) && dialog.result() == QDialog::Accepted,
        "Successful publication must close the dialog and accept the selection");
    const QByteArray promoted_provenance = read(game + "/stitching_canvas_provenance");
    QProcess program;
    QProcessEnvironment program_environment = QProcessEnvironment::systemEnvironment();
    program_environment.insert("HM_GAME_DIR", QFileInfo(game).absolutePath());
    program_environment.insert("HSTREAM_UI_PARENT_PID", QString::number(QCoreApplication::applicationPid()));
    program.setProcessEnvironment(program_environment);
    program.setWorkingDirectory(repo);
    program.setProcessChannelMode(QProcess::MergedChannels);
    QString program_output;
    QObject::connect(&program, &QProcess::readyReadStandardOutput, &program, [&] {
      const QString chunk = QString::fromLocal8Bit(program.readAllStandardOutput());
      program_output += chunk;
      log->appendPlainText(chunk);
    });
    program.start(
        runner,
        {"-g",
         QFileInfo(game).fileName(),
         "--enable-sources=URI-MULTIPLE",
         "--enable-sinks=FAKE",
         "-c",
         repo + "/configs/ds_hockey_app_config.yaml",
         "--start-time=" + anchor,
         "--stitch-frame-time=" + anchor,
         "--options=pipeline.hmaudio.enable=0,pipeline.hmplaycropper.show-scoreboard=0",
         "-t=5"});
    require(
        wait_until([&] { return program.state() == QProcess::NotRunning; }, 180000), "Main Program startup timed out");
    program_output += QString::fromLocal8Bit(program.readAllStandardOutput());
    require(
        program.exitStatus() == QProcess::NormalExit && program.exitCode() == 0 &&
            program_output.contains("App run successful") &&
            QRegularExpression("\\bfps(?:_avg)?=[1-9][0-9]*(?:\\.[0-9]+)?").match(program_output).hasMatch(),
        "Main Program must process video after promotion");
    require(
        !program_output.contains("stage=features status=started") &&
            !program_output.contains("captured stitching calibration frame pair") &&
            read(game + "/stitching_canvas_provenance") == promoted_provenance,
        "Main Program must reuse the promoted calibration without selecting or solving new frames");
    if (player_selection) {
      using namespace hm::stitching;
      const YAML::Node promoted = YAML::Load(read(game + "/config.yaml").toStdString());
      auto plan = ParsePlayerFrameSelectionPlan(promoted["stitching"]["calibration_frame_selection"]);
      require(
          plan.ok() &&
              promoted["stitching"]["calibration_frame_inputs_fingerprint"].as<std::string>("") == plan->fingerprint,
          "Promotion must publish the full-input marker with the exact selected plan");
      auto inputs = LoadPlayerFrameInputs(game.toStdString(), *plan);
      require(inputs.ok() && inputs->has_value(), "Promotion must retain every full-resolution selected PNG");
      const auto digests = [](const PlayerFrameInputSet& bundle) {
        std::vector<QByteArray> result;
        for (const auto& pair : bundle.images)
          for (const auto& path : pair)
            result.push_back(
                QCryptographicHash::hash(read(QString::fromStdString(path.string())), QCryptographicHash::Sha256));
        return result;
      };
      const auto selected_digests = digests(**inputs);
      StitchingExperimentDialog reopened(
          game,
          runner,
          repo,
          repo + "/configs/ds_hockey_app_config.yaml",
          experiment_environment,
          250,
          static_cast<int>(plan->selected.size()),
          anchor);
      reopened.show();
      auto* history = widget<QTableWidget>(reopened, "stitchExperimentCandidates");
      auto* reopened_log = widget<QPlainTextEdit>(reopened, "stitchExperimentLog");
      SaveLog reopened_save{reopened_log, log_path.isEmpty() ? QString() : log_path + ".reopen.log"};
      require(
          history->rowCount() >= expected_rows && history->item(0, 0)->text() == "Main calibration",
          "Reopened GPU dialog must show Main and all retained experiment rows");
      history->selectRow(0);
      bool main_inspected = false;
      QTimer::singleShot(0, &reopened, [&]() {
        if (auto* viewer = reopened.findChild<QDialog*>("stitchExperimentFrameInspector")) {
          auto* frames = viewer->findChild<QTableWidget*>("stitchExperimentSelectedFrames");
          auto* identity = viewer->findChild<QPlainTextEdit*>("stitchExperimentSelectedFrameIdentity");
          main_inspected = frames && frames->rowCount() == static_cast<int>(plan->selected.size()) && identity &&
              identity->toPlainText().contains(QString::fromStdString(plan->fingerprint));
          viewer->accept();
        }
      });
      widget<QPushButton>(reopened, "inspectStitchExperimentFramesButton")->click();
      require(
          main_inspected && reopened_log->toPlainText().isEmpty(),
          "Reopened Main inspection must not run any pipeline");
      const int before_add = history->rowCount();
      add_options(reopened);
      require(history->rowCount() == before_add + 1, "A reopened same-count variant must queue only one solve");
      const int log_offset = reopened_log->toPlainText().size();
      widget<QPushButton>(reopened, "startStitchExperimentBatchButton")->click();
      require(
          wait_until(
              [&] { return widget<QLabel>(reopened, "stitchExperimentStatus")->text().startsWith("Batch complete."); },
              600000),
          "Reopened GPU solve did not finish");
      require(
          history->item(before_add, 5)->text() == "Ready",
          "Retained full-resolution inputs must support another solve");
      const QString added_log = reopened_log->toPlainText().mid(log_offset);
      require(
          added_log.contains("retained calibration frame pairs") && added_log.contains("without recapture") &&
              !added_log.contains("captured stitching calibration frame pair") &&
              !added_log.contains("HSTREAM_PLAYER_SCAN"),
          "Reopened CP variant must reuse retained PNGs without baseline, scan or selected-frame extraction");
      const auto opened = OpenStitchingExperimentStore(game.toStdString());
      require(opened.ok(), "Reopened experiment catalog is unavailable");
      const auto catalog = LoadStitchingExperimentStore(*opened);
      require(catalog.ok(), "Reopened experiment catalog cannot be read");
      const auto last =
          std::find_if(catalog->experiments.rbegin(), catalog->experiments.rend(), [](const auto& candidate) {
            return candidate.workspace.settings.control_points == 250;
          });
      require(
          last != catalog->experiments.rend() && last->selection_fingerprint == plan->fingerprint,
          "New solve must retain the exact promoted fingerprint");
      const auto reused = LoadPlayerFrameInputs(last->workspace.game_directory, *plan);
      require(
          reused.ok() && reused->has_value() && digests(**reused) == selected_digests,
          "Every reused full-resolution PNG must match its promoted content digest");
      require(
          read(game + "/stitching_canvas_provenance") == promoted_provenance,
          "A retained-frame experiment must leave the promoted main geometry unchanged");
      history->selectRow(before_add);
      answer_close_guard(reopened, "stitchExperimentCloseDiscard", true);
      require(wait_until([&] { return !reopened.isVisible(); }, 1000), "Keeping reopened GPU results did not close");
    }
  } else {
    require(
        table->item(0, 5)->text() == "Runner exited 1" && table->item(1, 5)->text() == "Runner exited 1",
        "Failed candidate must not prevent the next queued run");
    dialog.reject();
    require(
        wait_until([&] { return !dialog.isVisible(); }, 1000),
        "Completed dialog must close without a forced-stop delay");
  }
}

} // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  try {
    // Real mode requires a disposable game copy: explicit selection changes it.
    if (argc == 5 && (QString(argv[1]) == "--gpu-smoke" || QString(argv[1]) == "--gpu-player-smoke")) {
      const QString repo = QString::fromLocal8Bit(argv[3]);
      exercise(
          QString::fromLocal8Bit(argv[2]),
          repo + "/bazel-bin/src/apps/hstream-cli/hstream-cli",
          repo,
          true,
          QString::fromLocal8Bit(argv[4]),
          QString(argv[1]) == "--gpu-player-smoke");
    } else {
      QTemporaryDir fixture;
      require(fixture.isValid(), "Cannot create test workspace");
      const auto make_game = [&](const char* name) {
        const QString game = fixture.path() + "/" + name;
        require(QDir().mkpath(game), "Cannot create fixture game");
        write(game + "/config.yaml", "game:\n  videos:\n    left: [left.mp4]\n    right: [right.mp4]\n");
        write(game + "/left.mp4", "left");
        write(game + "/right.mp4", "right");
        return game;
      };
      exercise_player_queue(make_game("queue"), fixture.path());
      exercise_queued_reopen(make_game("queued-reopen"), fixture.path());
      exercise_preparation_failure(make_game("partial-preparation"), fixture.path());
      exercise_saved_selection(make_game("saved"), fixture.path());
      exercise_close_promotion_failure(make_game("promotion"), fixture.path());
      exercise_player_cancellation(make_game("cancel"), fixture.path());
      exercise(make_game("ordinary"), "/bin/false", fixture.path(), false, {});
    }
    std::cout << "Stitching experiment workflow passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
