#include "src/apps/hstream-ui/StitchingExperimentDialog.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtGui/QScreen>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTableWidget>

#include <functional>
#include <iostream>
#include <stdexcept>

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

void exercise(const QString& game, const QString& runner, const QString& repo, bool gpu, const QString& log_path) {
  const QByteArray original_config = read(game + "/config.yaml");
  const QStringList original_files = QDir(game).entryList(QDir::Files);
  bool selected = false;
  StitchingExperimentDialog dialog(
      game,
      runner,
      repo,
      repo + "/configs/ds_hockey_app_config.yaml",
      QProcessEnvironment::systemEnvironment(),
      100,
      1,
      "00:00:00",
      nullptr,
      [&] { selected = true; });
  auto* log = widget<QPlainTextEdit>(dialog, "stitchExperimentLog");
  struct SaveLog {
    QPlainTextEdit* log;
    QString path;
    ~SaveLog() {
      if (!path.isEmpty()) {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly))
          file.write(log->toPlainText().toUtf8());
      }
    }
  } save_log{log, log_path};
  dialog.show();
  dialog.raise();
  QCoreApplication::processEvents();
  widget<QLineEdit>(dialog, "stitchExperimentControlPoints")->setText("100,150");
  widget<QLineEdit>(dialog, "stitchExperimentFrameCounts")->setText("1");
  widget<QLineEdit>(dialog, "stitchExperimentStartFrames")->setText("00:00:00");
  auto* add = widget<QPushButton>(dialog, "addStitchExperimentsToBatchButton");
  auto* start = widget<QPushButton>(dialog, "startStitchExperimentBatchButton");
  auto* table = widget<QTableWidget>(dialog, "stitchExperimentCandidates");
  add->click();
  add->click();
  require(table->rowCount() == 2 && start->isEnabled(), "Queue must deduplicate without running");
  require(read(game + "/config.yaml") == original_config, "Queue changed source config");
  start->click();
  auto* status = widget<QLabel>(dialog, "stitchExperimentStatus");
  require(
      wait_until([&] { return status->text().startsWith("Batch complete."); }, gpu ? 900000 : 10000),
      "Batch did not finish");
  require(read(game + "/config.yaml") == original_config, "Calibration changed source config");
  if (gpu) {
    widget<QCheckBox>(dialog, "stitchExperimentLoop")->setChecked(false);
    widget<QSpinBox>(dialog, "stitchExperimentPreviewDuration")->setValue(5);
    auto* play = widget<QPushButton>(dialog, "previewStitchExperimentButton");
    for (int row = 0; row < 2; ++row) {
      require(table->item(row, 5)->text() == "Ready", "Real calibration failed");
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
    auto* apply = widget<QPushButton>(dialog, "applyStitchExperimentButton");
    require(apply->isEnabled(), "Ready candidate cannot be selected");
    apply->click();
    require(wait_until([&] { return selected || apply->isEnabled(); }, 60000), "Candidate promotion timed out");
    if (!selected)
      throw std::runtime_error("Candidate promotion failed: " + status->text().toStdString());
    require(read(game + "/config.yaml") != original_config, "Explicit selection did not publish config");
  } else {
    require(
        table->item(0, 5)->text() == "Runner exited 1" && table->item(1, 5)->text() == "Runner exited 1",
        "Failed candidate must not prevent the next queued run");
  }
  dialog.reject();
  require(
      wait_until([&] { return !dialog.isVisible(); }, 1000), "Completed dialog must close without a forced-stop delay");
}

} // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  try {
    // Real mode requires a disposable game copy: explicit selection changes it.
    if (argc == 5 && QString(argv[1]) == "--gpu-smoke") {
      const QString repo = QString::fromLocal8Bit(argv[3]);
      exercise(
          QString::fromLocal8Bit(argv[2]),
          repo + "/bazel-bin/src/apps/hstream-cli/hstream-cli",
          repo,
          true,
          QString::fromLocal8Bit(argv[4]));
    } else {
      QTemporaryDir fixture;
      require(fixture.isValid(), "Cannot create test workspace");
      const QString game = fixture.path() + "/game";
      require(QDir().mkpath(game), "Cannot create fixture game");
      write(game + "/config.yaml", "game:\n  videos:\n    left: [left.mp4]\n    right: [right.mp4]\n");
      write(game + "/left.mp4", "left");
      write(game + "/right.mp4", "right");
      exercise(game, "/bin/false", fixture.path(), false, {});
    }
    std::cout << "Stitching experiment workflow passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
