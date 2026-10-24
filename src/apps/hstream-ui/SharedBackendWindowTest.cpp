#include "src/apps/hstream-ui/BackendPreview.h"
#include "src/apps/hstream-ui/HStreamWindow.h"
#include "src/apps/hstream-ui/NativePreviewSurface.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
void writeFile(const QString& path, const QByteArray& contents) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly), "Open fixture file");
  require(file.write(contents) == contents.size(), "Write fixture file");
}
bool waitFor(const std::function<bool()>& condition) {
  QElapsedTimer timer;
  timer.start();
  while (!condition() && timer.elapsed() < 10000)
    QTest::qWait(10);
  return condition();
}
} // namespace

struct HStreamWindowTestAccess {
  static void checkLifecycle(HStreamWindow& window, const QString& runner, const QString& frames) {
    window.show();
    for (const bool completed_receipt : {true, false}) {
      window.active_run_game_id_ = "game";
      window.active_hockeymon_program_ = true;
      window.backend_publication_complete_ = false;
      window.backend_publication_error_.clear();
      window.active_run_telemetry_requested_ = false;
      window.active_backend_output_kinds_.clear();
      window.preview_tabs_->setCurrentIndex(0);
      window.render_video_toggle_->setChecked(true);
      window.pipeline_render_embedded_ = true;
      auto environment = QProcessEnvironment::systemEnvironment();
      environment.insert("HM_HSTREAM_CALIBRATION_EXPORT", "/bin/true");
      environment.insert("TEST_FRAMES", frames);
      environment.insert("TEST_RECEIPT", completed_receipt ? "1" : "0");
      window.prepareBackendPreview(environment);
      window.preview_external_notice_->show();
      window.pipeline_process_->setProcessEnvironment(environment);
      window.pipeline_process_->start("/usr/bin/python3", {runner});
      const auto color = [](QWidget* target) {
        const QImage image = target->grab().toImage();
        return image.pixelColor(image.width() / 2, image.height() / 2);
      };
      require(
          waitFor([&] {
            return window.pipelineStateText() == "PLAYING" && window.preview_render_target_->isVisible() &&
                color(window.preview_render_target_).red() > 200;
          }),
          "Real backend process must display the first Program frame");
      require(color(window.preview_render_target_).red() > 200, "Program preview contains the producer's pixels");
      require(window.preview_external_notice_->isHidden(), "A received image removes the native waiting placeholder");
      require(
          waitFor([&] { return window.scoreboard_select_button_->isEnabled(); }),
          "Processed Program frames enable the shared scoreboard selector");
      require(
          window.scoreboard_controls_status_->text() == "Scoreboard placement also applies to recordings and streams.",
          "Program readiness replaces the idle scoreboard helper text");
      window.preview_tabs_->setCurrentIndex(1);
      require(
          waitFor([&] {
            return window.stitched_render_target_->isVisible() && color(window.stitched_render_target_).green() > 200;
          }),
          "Tab switching selects and paints the stitched producer frame");
      window.pipeline_process_->write("@test-finish\n");
      require(
          waitFor([&] { return window.pipeline_process_->state() == QProcess::NotRunning; }),
          "Backend process must finish");
      require(window.backend_publication_complete_ == completed_receipt, "Completion requires a backend receipt");
      require(
          window.playback_progress_state_ ==
              (completed_receipt ? HStreamWindow::PlaybackProgressState::kCompleted
                                 : HStreamWindow::PlaybackProgressState::kError),
          "Missing receipt must make a successful exit visibly fail");
      require(!window.preview_layout_compacted_, "Finishing restores the ordinary preview layout");
      require(
          window.stitched_render_target_->isVisible() && color(window.stitched_render_target_).green() > 200,
          "Final preview remains visible after success and failure");
    }
  }

  static void check(HStreamWindow& window, const QString& game_directory) {
    require(window.pipelineRunnerPath() == "/bin/true", "Program uses selected HockeyMON runner");
    require(window.backend_mode_->text().contains("hm-cli"), "Program identifies its Python backend");
    const QString native_runner = window.nativeRunnerPath();
    require(native_runner != "/bin/true", "Native workflow runner stays independent");
    window.run_mode_selector_->setCurrentIndex(window.run_mode_selector_->findData("stitch-calibration"));
    require(window.pipelineRunnerPath() == native_runner, "Calibration uses shared native workflow engine");
    require(window.backend_mode_->text().contains("hstream-cli"), "Calibration identifies its native backend");
    window.backend_.initial_launch = "/private/stitch-launch.json";
    require(window.pipelineRunnerPath() == "/bin/true", "Initial hmstitch arguments pass through the translating shim");
    window.active_run_game_id_ = window.initial_launch_game_ = "game";
    QProcessEnvironment native_environment;
    window.prepareBackendPreview(native_environment);
    require(
        native_environment.value("HM_UI_INITIAL_LAUNCH") == "/private/stitch-launch.json" &&
            native_environment.value("HSTREAM_NATIVE_RUNNER") == native_runner &&
            !native_environment.contains("HSTREAM_UI_BACKEND_SESSION"),
        "Calibration trampoline receives original launch and native runner without Python pixel ownership");
    require(
        !dynamic_cast<hm::ui::NativePreviewSurface*>(window.preview_render_target_)->imageMode(),
        "Initial native calibration retains GPU preview ownership");
    window.backend_.initial_launch.clear();
    window.run_mode_selector_->setCurrentIndex(window.run_mode_selector_->findData("program"));
    const QStringList arguments = window.pipelineArguments(false);
    require(
        std::any_of(
            arguments.begin(),
            arguments.end(),
            [](const auto& argument) { return argument.startsWith("--ui-preview-windows="); }),
        "Python preview contract remains available on offscreen Qt too");

    window.active_run_game_id_ = "game";
    window.initial_launch_game_ = "game";
    window.active_hockeymon_program_ = true;
    window.active_run_telemetry_requested_ = false;
    const QString config_path = QDir(game_directory).filePath("config.yaml");
    writeFile(config_path, "game:\n  videos:\n    source: [single.mp4]\n");
    bool calibration_required = true;
    require(
        window.prepareStitchingCalibrationRun("/must/not/run-native", game_directory, {}, &calibration_required) &&
            !calibration_required,
        "Single-source HockeyMON playback never starts native stitching calibration");
    QFile single_source_config(config_path);
    require(single_source_config.open(QIODevice::ReadOnly), "Read single-source game");
    require(
        single_source_config.readAll() == "game:\n  videos:\n    source: [single.mp4]\n",
        "Single-source admission must not mark native calibration pending");
    single_source_config.close();
    writeFile(config_path, "{}\n");
    for (auto& [kind, toggle] : window.output_toggles_)
      toggle->setChecked(false);
    QProcessEnvironment environment;
    environment.insert("HM_HSTREAM_CALIBRATION_EXPORT", "/bin/true");
    environment.insert("HSTREAM_UI_BACKEND_SESSION", "/must/not/reuse");
    window.prepareBackendPreview(environment);
    const QString session = environment.value("HSTREAM_UI_BACKEND_SESSION");
    require(
        !session.isEmpty() && session != "/must/not/reuse" && QFileInfo(session).isDir(),
        "Every run gets a fresh preview directory");
    require(environment.value("HSTREAM_NATIVE_RUNNER") == native_runner, "Adapter receives matching native runner");
    auto* target = dynamic_cast<hm::ui::NativePreviewSurface*>(window.preview_render_target_);
    require(target && target->imageMode(), "Shared preview switches ownership for Python");
    const QString path = QDir(game_directory).filePath("game_program-1.mp4");
    writeFile(path, "published-artifact-test-fixture");
    QJsonObject receipt{
        {"version", 1},
        {"status", "complete"},
        {"game_id", "game"},
        {"artifacts", QJsonArray{QJsonObject{{"kind", "program"}, {"path", path}}}}};
    const auto publish = [&] {
      return window.handleBackendPublication(
          "HM_BACKEND_PUBLICATION " + QString::fromUtf8(QJsonDocument(receipt).toJson(QJsonDocument::Compact)));
    };
    require(publish() && window.backend_publication_complete_, "Accept matching complete publication receipt");
    require(window.outputStateText("archive-file") == "SAVED", "Published Python archive is shown as saved");
    receipt["game_id"] = "another-game";
    require(publish() && !window.backend_publication_complete_, "Reject receipt for another game");
    receipt["game_id"] = "game";
    const QString explicit_output = QFileInfo(game_directory).dir().filePath("explicit-output.mp4");
    writeFile(explicit_output, "explicit destination fixture");
    receipt["artifacts"] = QJsonArray{QJsonObject{{"kind", "program"}, {"path", explicit_output}}};
    require(publish() && window.backend_publication_complete_, "Explicit output destinations may be outside the game");
    receipt["artifacts"] = QJsonArray{QJsonObject{{"kind", "program"}, {"path", "relative.mp4"}}};
    require(publish() && !window.backend_publication_complete_, "Reject relative publication paths");
    receipt["artifacts"] = QJsonArray{};
    window.active_backend_output_kinds_.append("archive-file");
    require(publish() && !window.backend_publication_complete_, "Missing requested archive cannot look successful");
    window.active_backend_output_kinds_.clear();
    window.output_toggles_.at("archive-file")->setChecked(true);
    require(publish() && window.backend_publication_complete_, "Display-only completion needs no archive artifact");
    window.output_toggles_.at("archive-file")->setChecked(false);

    YAML::Node config = YAML::Load("pipeline:\n  primary-gie:\n    model-engine-file: native.engine\n");
    const QString model = window.detectorModels().front().id;
    config["hstream_ui"]["hockeymon"]["detector"]["model"] = model.toStdString();
    config["hstream_ui"]["hockeymon"]["detector"]["precision"] = "int8";
    config["hstream_ui"]["hockeymon"]["detector"]["engine"] = "/private/python.onnx";
    config["hstream_ui"]["detector_int8"]["engine"] = "/private/native.engine";
    window.loadDetectorPrecision(config);
    require(
        window.detectorPrecision() == "int8" && window.prepared_int8_engine_ == "/private/python.onnx",
        "Python prepared detector is isolated from native TensorRT engine");
    const auto launch = window.pipelineArguments(true);
    require(
        launch.contains("--options=hstream_ui.hockeymon.detector.precision=int8"),
        "Unsaved backend precision reaches Program runner");
    require(
        !std::any_of(
            launch.begin(),
            launch.end(),
            [](const auto& argument) {
              return argument.startsWith("--options=pipeline.primary-gie.model-engine-file=");
            }),
        "Python detector launch never overrides native TensorRT engine field");
    window.active_run_game_id_.clear();
    window.active_hockeymon_program_ = false;
  }
};

int main(int argc, char** argv) {
  try {
    QTemporaryDir root;
    QTemporaryDir preferences;
    require(root.isValid() && preferences.isValid(), "Temporary game and preferences");
    const QString game = root.filePath("game");
    require(QDir().mkpath(game), "Create fixture game");
    writeFile(QDir(game).filePath("config.yaml"), "{}\n");
    qputenv("HM_GAME_DIR", root.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", preferences.path().toUtf8());
    QApplication app(argc, argv);
    HStreamWindow window(nullptr, {"hockeymon", "/bin/true"});
    window.openGame("game", false);
    HStreamWindowTestAccess::check(window, game);
    const QString runner = root.filePath("backend.py");
    writeFile(runner, R"PY(import json, os, pathlib, shutil, sys
session = pathlib.Path(os.environ['HSTREAM_UI_BACKEND_SESSION'])
for name in ('preview-final.jpg', 'preview-stitched.jpg'):
    shutil.copyfile(pathlib.Path(os.environ['TEST_FRAMES']) / name, session / name)
print('Pipeline running', flush=True)
print('HSTREAM_PREVIEW_RUNTIME status=ready channel=program generation=2', flush=True)
print('HSTREAM_PROGRESS instance=aggregate stage=0 instances=1 generation=0 processed_ns=1000000000 fps=10 fps_avg=10 speed_x=1', flush=True)
for line in sys.stdin:
    fields = line.strip().split()
    if fields[0] == '@set-preview-active':
        channel, generation = fields[1:]
        status = 'deactivated' if channel == 'none' else 'activated'
        print(f'HSTREAM_PREVIEW channel={channel} status={status} generation={generation} message=shared preview', flush=True)
        if channel != 'none':
            print(f'HSTREAM_PREVIEW channel={channel} status=ready generation={generation} message=first frame resolution=320x180', flush=True)
    elif fields[0] == '@test-finish':
        if os.environ['TEST_RECEIPT'] == '1':
            print('HM_BACKEND_PUBLICATION ' + json.dumps({'version': 1, 'game_id': 'game', 'status': 'complete', 'artifacts': []}), flush=True)
        break
)PY");
    QImage frame(320, 180, QImage::Format_RGB32);
    frame.fill(Qt::red);
    require(frame.save(root.filePath("preview-final.jpg")), "Write Program frame fixture");
    frame.fill(Qt::green);
    require(frame.save(root.filePath("preview-stitched.jpg")), "Write stitched frame fixture");
    HStreamWindowTestAccess::checkLifecycle(window, runner, root.path());
    std::cout << "Shared backend runner, publication, preview and detector isolation checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
