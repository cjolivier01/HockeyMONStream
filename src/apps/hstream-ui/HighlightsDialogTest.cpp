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
test -n "${HSTREAM_UI_PARENT_PID:-}"
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
  const QString short_video = temporary.filePath("short-video.mkv");
  if (!run(
          "ffmpeg",
          {"-hide_banner",
           "-loglevel",
           "error",
           "-y",
           "-f",
           "lavfi",
           "-i",
           "color=c=red:s=96x64:r=30:d=1",
           "-f",
           "lavfi",
           "-i",
           "sine=frequency=440:sample_rate=48000:duration=3",
           "-c:v",
           "libx264",
           "-pix_fmt",
           "yuv420p",
           "-c:a",
           "aac",
           short_video}))
    return 1;
  const QString short_video_game_dir = temporary.filePath("short-video-game");
  if (!QDir().mkpath(short_video_game_dir))
    return 1;
  QProcessEnvironment short_video_env = env;
  short_video_env.insert("HSTREAM_TEST_RED", short_video);
  hm::ui::HighlightsDialog short_video_dialog(
      "short-video-game",
      short_video_game_dir,
      runner,
      temporary.path(),
      temporary.path(),
      short_video_env,
      {"-g", "short-video-game"});
  if (!addRange(&short_video_dialog, "First", "00:00:00", "00:00:03"))
    return 1;
  short_video_dialog.findChild<QPushButton*>("highlightExportAllButton")->click();
  for (int i = 0; i < 3000 && short_video_dialog.isBusy(); ++i) {
    app.processEvents();
    QThread::msleep(10);
  }
  if (short_video_dialog.isBusy() ||
      !short_video_dialog.findChild<QLabel*>("highlightStatus")->text().contains("video ended before") ||
      QFileInfo::exists(QDir(short_video_game_dir).filePath("short-video-game-highlights-program-1.mp4"))) {
    std::cerr << "A long audio stream must not hide an incomplete video clip" << std::endl;
    return 1;
  }
  const QString eos_runner = temporary.filePath("eos-runner.sh");
  QFile eos_script(eos_runner);
  if (!eos_script.open(QIODevice::WriteOnly | QIODevice::Text))
    return 1;
  eos_script.write(R"(#!/bin/sh
set -eu
program=
stitched=
for arg in "$@"; do
  case "$arg" in
    --options=pipeline.sink2.output-file=*) program="${arg#--options=pipeline.sink2.output-file=}" ;;
    --options=pipeline.sink5.output-file=*) stitched="${arg#--options=pipeline.sink5.output-file=}" ;;
  esac
done
cp "$HSTREAM_TEST_RED" "$program"
cp "$HSTREAM_TEST_SHORT" "$stitched"
echo "HSTREAM_OUTPUT type=archive sink=2 kind=program path=$program"
echo "HSTREAM_OUTPUT type=archive sink=5 kind=stitched path=$stitched"
echo "HSTREAM_CLIP_RESULT reason=source-eos"
)");
  eos_script.close();
  if (!eos_script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
    return 1;
  const QString eos_game_dir = temporary.filePath("eos-game");
  if (!QDir().mkpath(eos_game_dir))
    return 1;
  QProcessEnvironment eos_env = env;
  eos_env.insert("HSTREAM_TEST_SHORT", short_video);
  hm::ui::HighlightsDialog eos_dialog(
      "eos-game", eos_game_dir, eos_runner, temporary.path(), temporary.path(), eos_env, {"-g", "eos-game"});
  if (!addRange(&eos_dialog, "First", "00:00:00", "00:00:03"))
    return 1;
  eos_dialog.findChild<QCheckBox*>("highlightStitchedCheck")->setChecked(true);
  eos_dialog.findChild<QPushButton*>("highlightExportAllButton")->click();
  for (int i = 0; i < 3000 && eos_dialog.isBusy(); ++i) {
    app.processEvents();
    QThread::msleep(10);
  }
  if (eos_dialog.isBusy() ||
      !eos_dialog.findChild<QLabel*>("highlightStatus")->text().contains("different video lengths") ||
      QFileInfo::exists(QDir(eos_game_dir).filePath("eos-game-highlights-program-1.mp4"))) {
    std::cerr << "Source EOS must not publish outputs with different video lengths" << std::endl;
    return 1;
  }
  const QString primed_video = temporary.filePath("primed-video.mkv");
  if (!run(
          "ffmpeg",
          {"-hide_banner",
           "-loglevel",
           "error",
           "-y",
           "-f",
           "lavfi",
           "-i",
           "color=c=red:s=96x64:r=60:d=1",
           "-f",
           "lavfi",
           "-i",
           "sine=frequency=440:sample_rate=48000:duration=1",
           "-c:v",
           "libx264",
           "-bf",
           "0",
           "-c:a",
           "aac",
           primed_video}))
    return 1;
  const QString primed_game_dir = temporary.filePath("primed-game");
  if (!QDir().mkpath(primed_game_dir))
    return 1;
  QProcessEnvironment primed_env = env;
  primed_env.insert("HSTREAM_TEST_RED", primed_video);
  primed_env.insert("HSTREAM_TEST_BLUE", primed_video);
  hm::ui::HighlightsDialog primed_dialog(
      "primed-game", primed_game_dir, runner, temporary.path(), temporary.path(), primed_env, {"-g", "primed-game"});
  if (!addRange(&primed_dialog, "First", "00:00:00", "00:00:01") ||
      !addRange(&primed_dialog, "Second", "00:00:10", "00:00:11"))
    return 1;
  primed_dialog.findChild<QPushButton*>("highlightExportAllButton")->click();
  const QString primed_output = QDir(primed_game_dir).filePath("primed-game-highlights-program-1.mp4");
  if (!waitForExport(&app, &primed_dialog, primed_output, 3000))
    return 1;
  QByteArray packet_count;
  if (!run(
          "ffprobe",
          {"-v",
           "error",
           "-select_streams",
           "v:0",
           "-count_packets",
           "-show_entries",
           "stream=nb_read_packets",
           "-of",
           "default=noprint_wrappers=1:nokey=1",
           primed_output},
          &packet_count) ||
      packet_count.trimmed() != "120") {
    std::cerr << "AAC priming must not drop the last video packet from either clip" << std::endl;
    return 1;
  }
  const QString signal_runner = temporary.filePath("signal-runner.sh");
  QFile signal_script(signal_runner);
  if (!signal_script.open(QIODevice::WriteOnly | QIODevice::Text))
    return 1;
  signal_script.write(R"(#!/bin/sh
set -eu
test -n "${HSTREAM_UI_PARENT_PID:-}"
printf '%s\n' "$*" >> "$HSTREAM_TEST_CALLS"
trap 'printf INT > "$HSTREAM_TEST_SIGNAL"; echo "HSTREAM_CLIP_RESULT reason=end-boundary"; exit 0' INT
trap 'printf TERM > "$HSTREAM_TEST_SIGNAL"; exit 0' TERM
printf ready > "$HSTREAM_TEST_READY"
while :; do sleep 0.1; done
)");
  signal_script.close();
  if (!signal_script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
    return 1;
  const QString signal_game_dir = temporary.filePath("signal-game");
  if (!QDir().mkpath(signal_game_dir))
    return 1;
  QProcessEnvironment signal_env = env;
  const QString signal_path = temporary.filePath("signal.txt");
  const QString ready_path = temporary.filePath("ready.txt");
  signal_env.insert("HSTREAM_TEST_SIGNAL", signal_path);
  signal_env.insert("HSTREAM_TEST_READY", ready_path);
  hm::ui::HighlightsDialog signal_dialog(
      "signal-game",
      signal_game_dir,
      signal_runner,
      temporary.path(),
      temporary.path(),
      signal_env,
      {"-g", "signal-game"});
  if (!addRange(&signal_dialog, "First", "00:00:00", "00:00:01"))
    return 1;
  signal_dialog.findChild<QPushButton*>("highlightPreviewSelectedButton")->click();
  for (int i = 0; i < 1000 && !QFileInfo::exists(ready_path); ++i) {
    app.processEvents();
    QThread::msleep(1);
  }
  if (!QFileInfo::exists(ready_path)) {
    std::cerr << "The signal fixture did not start" << std::endl;
    return 1;
  }
  signal_dialog.findChild<QPushButton*>("highlightStopButton")->click();
  for (int i = 0; i < 3000 && signal_dialog.isBusy(); ++i) {
    app.processEvents();
    QThread::msleep(1);
  }
  QFile signal_file(signal_path);
  if (signal_dialog.isBusy() || !signal_file.open(QIODevice::ReadOnly) || signal_file.readAll() != "INT") {
    std::cerr << "Stopping a clip must request graceful SIGINT shutdown" << std::endl;
    return 1;
  }
  signal_file.close();
  QFile::remove(signal_path);
  QFile::remove(ready_path);
  signal_dialog.findChild<QPushButton*>("highlightPreviewSelectedButton")->click();
  signal_dialog.findChild<QPushButton*>("highlightStopButton")->click();
  for (int i = 0; i < 3000 && signal_dialog.isBusy(); ++i) {
    app.processEvents();
    QThread::msleep(1);
  }
  if (signal_dialog.isBusy()) {
    std::cerr << "Stopping while the runner starts must still cancel the job" << std::endl;
    return 1;
  }
  const auto signal_call_count = [&calls]() {
    QFile file(calls);
    if (!file.open(QIODevice::ReadOnly))
      return -1;
    return static_cast<int>(QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts).size());
  };
  const int calls_before_teardown = signal_call_count();
  const QString teardown_game_dir = temporary.filePath("teardown-game");
  if (calls_before_teardown < 0 || !QDir().mkpath(teardown_game_dir))
    return 1;
  QFile::remove(ready_path);
  auto* teardown_dialog = new hm::ui::HighlightsDialog(
      "teardown-game",
      teardown_game_dir,
      signal_runner,
      temporary.path(),
      temporary.path(),
      signal_env,
      {"-g", "teardown-game"});
  if (!addRange(teardown_dialog, "First", "00:00:00", "00:00:01") ||
      !addRange(teardown_dialog, "Second", "00:00:10", "00:00:11")) {
    delete teardown_dialog;
    return 1;
  }
  teardown_dialog->findChild<QPushButton*>("highlightPreviewAllButton")->click();
  for (int i = 0; i < 1000 && !QFileInfo::exists(ready_path); ++i) {
    app.processEvents();
    QThread::msleep(1);
  }
  if (!QFileInfo::exists(ready_path)) {
    delete teardown_dialog;
    std::cerr << "The teardown fixture did not start" << std::endl;
    return 1;
  }
  delete teardown_dialog;
  app.processEvents();
  if (signal_call_count() != calls_before_teardown + 1) {
    std::cerr << "Dialog teardown must not launch the next clip" << std::endl;
    return 1;
  }
  return 0;
}
