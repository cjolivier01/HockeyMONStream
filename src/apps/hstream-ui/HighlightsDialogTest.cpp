#include "src/apps/hstream-ui/HighlightsDialog.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>

#include <cmath>
#include <iostream>

namespace {

bool run(const QString& program, const QStringList& args, QByteArray* output = nullptr) {
  QProcess process;
  process.start(program, args);
  if (!process.waitForFinished(30000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    std::cerr << process.readAllStandardError().constData() << std::endl;
    return false;
  }
  if (output)
    *output = process.readAllStandardOutput();
  return true;
}

bool sample(const QString& path, const QString& color, int frequency) {
  return run("ffmpeg", {"-hide_banner", "-loglevel",
                        "error",        "-y",
                        "-f",           "lavfi",
                        "-i",           "color=c=" + color + ":s=96x64:r=30:d=3",
                        "-f",           "lavfi",
                        "-i",           QString("sine=frequency=%1:sample_rate=48000:duration=3").arg(frequency),
                        "-c:v",         "libx264",
                        "-pix_fmt",     "yuv420p",
                        "-g",           "30",
                        "-c:a",         "aac",
                        "-shortest",    path});
}

bool addRange(hm::ui::HighlightsDialog* dialog, const QString& name, const QString& start, const QString& end) {
  auto* label = dialog->findChild<QLineEdit*>("highlightLabelEdit");
  auto* first = dialog->findChild<QLineEdit*>("highlightFirstTimeEdit");
  auto* second = dialog->findChild<QLineEdit*>("highlightSecondTimeEdit");
  auto* mode = dialog->findChild<QComboBox*>("highlightModeCombo");
  auto* add = dialog->findChild<QPushButton*>("highlightAddButton");
  if (!label || !first || !second || !mode || !add)
    return false;
  mode->setCurrentIndex(1);
  label->setText(name);
  first->setText(start);
  second->setText(end);
  add->click();
  return dialog->findChild<QTableWidget*>("highlightsTable")->rowCount() == (name == "First" ? 1 : 2);
}

bool waitForExport(QApplication* app, hm::ui::HighlightsDialog* dialog, const QString& output, int maximum_iterations) {
  for (int i = 0; i < maximum_iterations && (dialog->isBusy() || !QFileInfo::exists(output)); ++i) {
    app->processEvents();
    QThread::msleep(10);
  }
  if (!dialog->isBusy() && QFileInfo::exists(output))
    return true;
  std::cerr << "Export did not finish: " << dialog->findChild<QLabel*>("highlightStatus")->text().toStdString()
            << std::endl;
  return false;
}

int realGameE2E(QApplication* app) {
  const QString game_dir = qEnvironmentVariable("HSTREAM_HIGHLIGHTS_E2E_GAME_DIR");
  const QString runner = qEnvironmentVariable("HSTREAM_HIGHLIGHTS_E2E_RUNNER");
  const QString working_dir = qEnvironmentVariable("HSTREAM_HIGHLIGHTS_E2E_WORKING_DIR");
  if (!QFileInfo(runner).isFile() || !QFileInfo(game_dir).isDir() || working_dir.isEmpty()) {
    std::cerr << "E2E runner, game directory, or working directory is missing" << std::endl;
    return 1;
  }
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert("HM_GAME_DIR", QFileInfo(game_dir).absolutePath());
  const QStringList base_args = {
      "-g",
      "tv-14-1-p1",
      "--enable-sources=URI-MULTIPLE",
      "-c",
      QDir(working_dir).filePath("configs/ds_hockey_app_config.yaml"),
      "--options=pipeline.hmaudio.enable=1",
      "--options=pipeline.hmstitcher.private-properties.calibrate-field-mask=0",
      "--options=pipeline.ds-fieldmask.enable=0"};
  hm::ui::HighlightsDialog dialog(
      "tv-14-1-p1",
      game_dir,
      runner,
      working_dir,
      qEnvironmentVariable("HSTREAM_HIGHLIGHTS_E2E_OUTPUT_ROOT", QDir::tempPath()),
      env,
      base_args);
  if (!addRange(&dialog, "First", "00:00:05", "00:00:07") || !addRange(&dialog, "Second", "00:00:15", "00:00:17"))
    return 1;
  auto* base_name = dialog.findChild<QLineEdit*>("highlightBaseNameEdit");
  base_name->setText("e2e-" + QString::number(QDateTime::currentMSecsSinceEpoch()));
  dialog.findChild<QCheckBox*>("highlightProgram4kCheck")->setChecked(true);
  dialog.findChild<QCheckBox*>("highlightStitchedCheck")->setChecked(true);
  dialog.findChild<QPushButton*>("highlightExportAllButton")->click();
  for (const QString route : {"program", "program_4k", "stitched"}) {
    const QString output = QDir(game_dir).filePath(QString("tv-14-1-p1-%1-%2-1.mp4").arg(base_name->text(), route));
    if (!waitForExport(app, &dialog, output, 90000))
      return 1;
    QByteArray probe;
    if (!run("ffprobe", {"-v", "error", "-show_streams", "-show_format", "-of", "json", output}, &probe))
      return 1;
    const QJsonObject root = QJsonDocument::fromJson(probe).object();
    const double duration = root.value("format").toObject().value("duration").toString().toDouble();
    if (std::abs(duration - 4.0) > 0.5 || root.value("streams").toArray().size() != 2) {
      std::cerr << "Unexpected " << route.toStdString() << " duration or stream layout: " << probe.constData()
                << std::endl;
      return 1;
    }
    std::cout << route.toStdString() << " " << output.toStdString() << " duration=" << duration << std::endl;
  }
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  if (qEnvironmentVariableIsSet("HSTREAM_HIGHLIGHTS_E2E_GAME_DIR"))
    return realGameE2E(&app);
  if (QStandardPaths::findExecutable("ffmpeg").isEmpty() || QStandardPaths::findExecutable("ffprobe").isEmpty()) {
    std::cerr << "ffmpeg and ffprobe are required" << std::endl;
    return 1;
  }
  QTemporaryDir temporary(QDir::tempPath() + "/hstream-highlights-test-XXXXXX");
  if (!temporary.isValid())
    return 1;
  const QString red = temporary.filePath("red.mkv");
  const QString blue = temporary.filePath("blue.mkv");
  if (!sample(red, "red", 440) || !sample(blue, "blue", 660))
    return 1;
  const QString game_dir = temporary.filePath("test-game");
  if (!QDir().mkpath(game_dir))
    return 1;
  const QString runner = temporary.filePath("fake-runner.sh");
  QFile script(runner);
  if (!script.open(QIODevice::WriteOnly | QIODevice::Text))
    return 1;
  script.write(R"(#!/bin/sh
set -eu
out=
sample="$HSTREAM_TEST_RED"
echo "$*" >> "$HSTREAM_TEST_CALLS"
for arg in "$@"; do
  case "$arg" in
    --start-time=00:00:10*) sample="$HSTREAM_TEST_BLUE" ;;
    --options=pipeline.sink2.output-file=*) out="${arg#--options=pipeline.sink2.output-file=}" ;;
  esac
done
if [ -z "$out" ]; then
  echo "HSTREAM_CLIP_RESULT reason=end-boundary"
  exit 0
fi
cp "$sample" "$out"
echo "HSTREAM_OUTPUT type=archive sink=2 kind=program existed=0 size=0 mtime-ms=-1 codec=h264 path=$out"
echo "HSTREAM_CLIP_RESULT reason=end-boundary"
)");
  script.close();
  if (!script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
    return 1;
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert("HSTREAM_TEST_RED", red);
  env.insert("HSTREAM_TEST_BLUE", blue);
  const QString calls = temporary.filePath("calls.log");
  env.insert("HSTREAM_TEST_CALLS", calls);
  hm::ui::HighlightsDialog dialog(
      "test-game", game_dir, runner, temporary.path(), temporary.path(), env, {"-g", "test-game"});
  if (!addRange(&dialog, "First", "00:00:00", "00:00:01") || !addRange(&dialog, "Second", "00:00:10", "00:00:11")) {
    std::cerr << "Could not add two ranges" << std::endl;
    return 1;
  }
  auto* export_all = dialog.findChild<QPushButton*>("highlightExportAllButton");
  if (!export_all)
    return 1;
  export_all->click();
  const QString output = QDir(game_dir).filePath("test-game-highlights-program-1.mp4");
  if (!waitForExport(&app, &dialog, output, 3000))
    return 1;
  QByteArray probe;
  if (!run("ffprobe", {"-v", "error", "-show_streams", "-show_format", "-of", "json", output}, &probe))
    return 1;
  const QJsonObject root = QJsonDocument::fromJson(probe).object();
  const double duration = root.value("format").toObject().value("duration").toString().toDouble();
  if (std::abs(duration - 2.0) > 0.3 || root.value("streams").toArray().size() != 2) {
    std::cerr << "Unexpected joined streams or duration: " << probe.constData() << std::endl;
    return 1;
  }
  for (const auto& sample_time : {std::pair<QString, bool>{"0.2", true}, {"1.3", false}}) {
    QByteArray pixel;
    if (!run(
            "ffmpeg",
            {"-hide_banner",
             "-loglevel",
             "error",
             "-ss",
             sample_time.first,
             "-i",
             output,
             "-frames:v",
             "1",
             "-vf",
             "scale=1:1",
             "-pix_fmt",
             "rgb24",
             "-f",
             "rawvideo",
             "pipe:1"},
            &pixel) ||
        pixel.size() != 3)
      return 1;
    const unsigned char red_value = static_cast<unsigned char>(pixel[0]);
    const unsigned char blue_value = static_cast<unsigned char>(pixel[2]);
    if (sample_time.second ? red_value < blue_value + 50 : blue_value < red_value + 50) {
      std::cerr << "Joined clips are out of order" << std::endl;
      return 1;
    }
  }
  export_all->click();
  if (!waitForExport(&app, &dialog, QDir(game_dir).filePath("test-game-highlights-program-2.mp4"), 3000))
    return 1;
  auto* preview_all = dialog.findChild<QPushButton*>("highlightPreviewAllButton");
  preview_all->click();
  for (int i = 0; i < 3000 && dialog.isBusy(); ++i) {
    app.processEvents();
    QThread::msleep(1);
  }
  QFile call_log(calls);
  if (dialog.isBusy() || !call_log.open(QIODevice::ReadOnly))
    return 1;
  const QStringList calls_list = QString::fromUtf8(call_log.readAll()).split('\n', Qt::SkipEmptyParts);
  if (calls_list.size() < 6 || !calls_list[calls_list.size() - 2].contains("--enable-sinks=RENDER") ||
      !calls_list[calls_list.size() - 2].contains("--start-time=00:00:00") ||
      !calls_list.last().contains("--enable-sinks=RENDER") || !calls_list.last().contains("--start-time=00:00:10")) {
    std::cerr << "Preview did not play both intervals with render-only output" << std::endl;
    return 1;
  }
  dialog.findChild<QTableWidget*>("highlightsTable")->setCurrentCell(0, 0);
  dialog.findChild<QPushButton*>("highlightLoopSelectedButton")->click();
  app.processEvents();
  dialog.findChild<QPushButton*>("highlightStopButton")->click();
  for (int i = 0; i < 1000 && dialog.isBusy(); ++i) {
    app.processEvents();
    QThread::msleep(1);
  }
  if (dialog.isBusy()) {
    std::cerr << "Loop stop did not cancel the queued preview" << std::endl;
    return 1;
  }
  const QString incompatible_game_dir = temporary.filePath("incompatible-game");
  if (!QDir().mkpath(incompatible_game_dir))
    return 1;
  const QString incompatible_plan_path = QDir(incompatible_game_dir).filePath("highlights.json");
  const QByteArray incompatible_plan = R"({"schema":2,"base_name":"future","intervals":[]})";
  QFile incompatible_file(incompatible_plan_path);
  if (!incompatible_file.open(QIODevice::WriteOnly) ||
      incompatible_file.write(incompatible_plan) != incompatible_plan.size())
    return 1;
  incompatible_file.close();
  hm::ui::HighlightsDialog incompatible_dialog(
      "incompatible-game",
      incompatible_game_dir,
      runner,
      temporary.path(),
      temporary.path(),
      env,
      {"-g", "incompatible-game"});
  if (incompatible_dialog.findChild<QPushButton*>("highlightAddButton")->isEnabled() ||
      incompatible_dialog.findChild<QLineEdit*>("highlightBaseNameEdit")->isEnabled() ||
      !incompatible_dialog.findChild<QLabel*>("highlightStatus")->text().contains("Unsupported highlights schema")) {
    std::cerr << "An unreadable highlight plan must block edits" << std::endl;
    return 1;
  }
  incompatible_dialog.findChild<QPushButton*>("highlightAddButton")->click();
  if (!incompatible_file.open(QIODevice::ReadOnly) || incompatible_file.readAll() != incompatible_plan) {
    std::cerr << "Editing an unreadable plan must not replace it" << std::endl;
    return 1;
  }
  return 0;
}
