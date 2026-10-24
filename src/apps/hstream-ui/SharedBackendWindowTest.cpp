#include "src/apps/hstream-ui/BackendPreview.h"
#include "src/apps/hstream-ui/HStreamWindow.h"
#include "src/apps/hstream-ui/NativePreviewSurface.h"
#include "src/apps/hstream-ui/ScoreboardSelectionDialog.h"

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
bool waitFor(const std::function<bool()>& condition, int timeout_ms = 10000) {
  QElapsedTimer timer;
  timer.start();
  while (!condition() && timer.elapsed() < timeout_ms)
    QTest::qWait(10);
  return condition();
}
} // namespace

struct HStreamWindowTestAccess {
  static void stopRealBackend(HStreamWindow& window) {
    window.stopPipeline();
  }
  // Opt-in integration driver: use a disposable real game and the actual
  // HockeyMON runner. The ordinary hermetic test below never enables it.
  static void checkRealBackend(HStreamWindow& window, const QString& game) {
    const QString application = QDir(QCoreApplication::applicationDirPath()).filePath("hstream-ui");
    window.development_bazel_bin_ = hm::ui_internal::matching_development_bazel_bin(application);
    window.development_runtime_root_ = hm::ui_internal::development_runtime_root_for_application(application);
    window.development_pipeline_runner_ = hm::ui_internal::matching_development_pipeline_runner(application);
    require(
        !window.development_bazel_bin_.isEmpty() && !window.development_runtime_root_.isEmpty() &&
            !window.development_pipeline_runner_.isEmpty(),
        "Real integration checks require the matching built HStream tools");
    const bool preparation_stop = qEnvironmentVariableIsSet("HM_UI_E2E_STOP_PREPARATION");
    const bool render_off_first = qEnvironmentVariableIsSet("HM_UI_E2E_RENDER_OFF_FIRST");
    const bool initial_launch = !window.backend_.initial_launch.isEmpty();
    const int cameras = qEnvironmentVariableIntValue("HM_UI_E2E_CAMERAS");
    const QString captures = qEnvironmentVariable("HM_UI_E2E_CAPTURES");
    const auto paintsSourcePixels = [](QWidget* target) {
      // Ready receipts can arrive before the image polling timer paints the
      // first frame. Inspect the rendered surface so a black placeholder cannot
      // satisfy the real integration check or enter its screenshot evidence.
      const QImage painted = target->grab().toImage();
      if (painted.isNull())
        return false;
      int bright_samples = 0;
      for (int row = 2; row < 6; ++row)
        for (int column = 2; column < 6; ++column) {
          const QColor pixel = painted.pixelColor(painted.width() * column / 8, painted.height() * row / 8);
          if (qGray(pixel.rgb()) > 20)
            ++bright_samples;
        }
      return bright_samples >= 4;
    };
    window.resize(1560, 960);
    window.show();
    window.openGame(game, false);
    window.render_video_toggle_->setChecked(!render_off_first);
    window.realtime_playback_toggle_->setChecked(true);
    window.drivegpt_csv_toggle_->setChecked(!initial_launch);
    for (auto& [kind, toggle] : window.output_toggles_)
      toggle->setChecked(!initial_launch && (kind == "archive-file" || kind == "archive-stitched"));
    window.preview_tabs_->setCurrentIndex(0);
    window.start_button_->click();
    require(
        waitFor([&] { return window.pipeline_process_->state() == QProcess::Running; }, 30000),
        "Actual backend must start");
    const auto pid = window.pipeline_process_->processId();
    if (preparation_stop) {
      require(
          waitFor([&] { return window.log_->toPlainText().contains("Starting shared native calibration"); }, 30000),
          "Actual native preparation must begin before Stop");
    } else {
      if (render_off_first) {
        require(
            waitFor([&] { return window.preview_runtime_ready_; }, 180000),
            "Render-off startup must announce command readiness without an image");
        require(
            !QFileInfo::exists(window.backend_preview_directory_->filePath("preview-final.jpg")),
            "Render-off startup does not publish preview frames");
        window.render_video_toggle_->setChecked(true);
      }
      require(
          waitFor(
              [&] {
                return window.pipeline_process_->state() == QProcess::NotRunning ||
                    (window.backend_preview_directory_ &&
                     QFileInfo(window.backend_preview_directory_->filePath("preview-final.jpg")).size() > 0);
              },
              180000),
          "Actual Python Program must publish pixels");
      require(
          window.pipeline_process_->state() == QProcess::Running, "Actual Python Program must remain active for Stop");
      for (int camera = 0; camera < cameras; ++camera) {
        window.preview_tabs_->setCurrentIndex(2 + camera);
        require(
            waitFor(
                [&] {
                  return QFileInfo(window.backend_preview_directory_->filePath(
                                       QString("preview-camera-%1.jpg").arg(camera + 1)))
                             .size() > 0 &&
                      window.camera_preview_render_targets_[camera]->isVisible() &&
                      window.camera_preview_notices_[camera]->isHidden() &&
                      paintsSourcePixels(window.camera_preview_render_targets_[camera]);
                },
                30000),
            "Selecting an actual source tab must receive and display Python source pixels");
        if (!captures.isEmpty()) {
          require(QDir().mkpath(captures), "Create external screenshot directory");
          require(
              window.grab().save(QDir(captures).filePath(QString("source-%1.png").arg(camera + 1))),
              "Save external source screenshot");
        }
      }
      window.preview_tabs_->setCurrentIndex(0);
      QTest::qWait(1000);
      if (qEnvironmentVariableIsSet("HM_UI_E2E_SCOREBOARD")) {
        require(waitFor([&] { return window.pause_button_->isEnabled(); }), "Pause is available for scoreboard E2E");
        window.pause_button_->click();
        require(waitFor([&] { return window.pipeline_paused_; }), "Actual Program acknowledges pause");
        require(window.pipeline_cooperative_pause_, "Actual Python pause retains the supervisor event loop");
        const qint64 paused_position = window.playback_position_ns_;
        QTest::qWait(500);
        require(window.playback_position_ns_ == paused_position, "Acknowledged pause does not advance Program frames");
        require(window.scoreboard_select_button_->isEnabled(), "Paused Program permits scoreboard selection");
        window.scoreboard_select_button_->click();
        require(
            waitFor([&] { return window.scoreboard_selection_dialog_ != nullptr; }, 180000),
            "Actual paused Program opens its source scoreboard selector");
        auto* dialog = window.scoreboard_selection_dialog_;
        require(dialog->loadError().isEmpty(), "Actual source canvas loads without using saved panorama dimensions");
        auto* canvas =
            static_cast<ScoreboardSelectionCanvas*>(dialog->findChild<QWidget*>("scoreboardSelectionCanvas"));
        require(canvas && canvas->imageSize() == QSize(1920, 1080), "Selector uses the actual source dimensions");
        auto* save = dialog->findChild<QPushButton*>("scoreboardSaveButton");
        require(save && save->text() == "Apply to This Run", "Override selection clearly identifies its run scope");
        canvas->setPoints({{10, 10}, {110, 10}, {110, 60}, {10, 60}});
        if (!captures.isEmpty())
          require(
              dialog->grab().save(QDir(captures).filePath("source-scoreboard-paused.png")),
              "Save paused source selector screenshot");
        save->click();
        require(
            waitFor([&] { return window.scoreboard_selection_dialog_ == nullptr; }, 15000),
            "Paused frame owner acknowledges the actual source selection");
        require(window.pipeline_paused_, "Scoreboard save keeps the operator's pause");
        const auto selected =
            YAML::LoadFile(window.backend_preview_directory_->filePath("scoreboard-source/config.yaml").toStdString());
        require(
            selected["rink"]["scoreboard"]["perspective_polygon"][0][0].as<int>() == 10,
            "Native source transaction persists the selected run polygon");
        window.pause_button_->click();
        require(
            waitFor([&] { return !window.pipeline_paused_ && window.pending_process_resume_generation_ == 0; }),
            "Actual Program resumes after selection");
        require(
            waitFor([&] { return window.playback_position_ns_ > paused_position; }, 15000),
            "Resumed Program advances actual frames");
        window.pause_button_->click();
        require(waitFor([&] { return window.pipeline_paused_; }), "Pause again before Stop/publication");
      }
    }
    window.stop_button_->click();
    require(window.pipeline_process_->state() == QProcess::NotRunning, "Actual Stop waits for every processing child");
    require(window.pipeline_process_->exitCode() == 0, "Cooperative Stop permits successful worker finalization");
    if (preparation_stop) {
      require(
          !window.log_->toPlainText().contains("Starting HockeyMON Python pipeline"),
          "Stopping preparation never starts the Python worker");
      require(
          window.outputStateText("archive-file") == "NO FILE" &&
              window.outputStateText("archive-stitched") == "NO FILE",
          "Preparation cancellation never claims requested archives were saved");
    } else {
      require(window.backend_publication_complete_, "Actual Stop receives a complete publication receipt");
      require(window.outputStateText("archive-file") == "SAVED", "Actual Stop publishes the requested Program archive");
      if (!initial_launch)
        require(
            window.outputStateText("archive-stitched") == "SAVED",
            "Actual Stop publishes the requested stitched archive");
    }
    std::cout << "REAL_QT_BACKEND_OK game=" << game.toStdString() << " pid=" << pid
              << " preparation_stop=" << preparation_stop << '\n';
    std::cout << window.log_->toPlainText().toStdString() << '\n';
  }

  static void checkResolvedLaunch(HStreamWindow& window) {
    window.active_run_game_id_ = "game";
    window.active_hockeymon_program_ = true;
    window.active_backend_initial_launch_ = true;
    window.backend_launch_received_ = false;
    window.backend_launch_failed_ = false;
    window.active_backend_output_kinds_.clear();
    window.active_run_telemetry_requested_ = false;
    window.active_run_local_render_only_ = true;
    QJsonObject resolved{
        {"version", 1}, {"game_id", "game"}, {"sinks", QJsonArray{"RENDER", "ENCODE_FILE"}}, {"telemetry", false}};
    const auto launch = [&] {
      return window.handleBackendLaunch(
          "HM_BACKEND_LAUNCH " + QString::fromUtf8(QJsonDocument(resolved).toJson(QJsonDocument::Compact)));
    };
    require(launch() && window.backend_launch_received_, "Initial Python output intent is admitted");
    require(
        window.active_backend_output_kinds_ == QStringList{"archive-file"} &&
            window.output_toggles_.at("archive-file")->isChecked() && !window.active_run_local_render_only_,
        "Initial output request updates receipt requirements, display and seek admission");
    require(
        window.handleBackendPublication(
            "HM_BACKEND_PUBLICATION {\"version\":1,\"game_id\":\"game\",\"status\":\"complete\",\"artifacts\":[]}") &&
            !window.backend_publication_complete_,
        "An explicit initial output requires its actual published artifact");
    const QString cancelled =
        "HM_BACKEND_PUBLICATION {\"version\":1,\"game_id\":\"game\",\"status\":\"cancelled\","
        "\"stage\":\"preparing\",\"artifacts\":[]}";
    window.pipeline_stop_requested_ = false;
    require(
        window.handleBackendPublication(cancelled) && !window.backend_publication_complete_,
        "Unrequested cancellation cannot report successful completion");
    window.pipeline_stop_requested_ = true;
    require(
        window.handleBackendPublication(cancelled) && window.backend_publication_complete_ &&
            window.outputStateText("archive-file") == "NO FILE",
        "Operator cancellation before processing reports no recording instead of missing publication");
    window.pipeline_stop_requested_ = false;
    window.backend_launch_received_ = false;
    resolved["sinks"] = QJsonArray{"RENDER"};
    require(launch() && window.active_backend_output_kinds_.isEmpty(), "Initial no-save overrides checked UI output");
    require(!window.output_toggles_.at("archive-file")->isChecked(), "Initial no-save is reflected in the UI");
    window.backend_launch_received_ = false;
    window.active_backend_initial_launch_ = false;
    resolved["sinks"] = QJsonArray{"RENDER", "ENCODE_FILE"};
    require(launch() && window.backend_launch_failed_, "Ordinary launches cannot replace the desktop output request");
    window.backend_launch_received_ = false;
    window.backend_launch_failed_ = false;
    window.backend_publication_error_.clear();
    window.active_backend_output_kinds_.clear();
  }

  static void checkLifecycle(HStreamWindow& window, const QString& runner, const QString& frames) {
    window.show();
    for (const bool completed_receipt : {true, false}) {
      window.resetPlaybackProgress(true);
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
      environment.insert("HM_HSTREAM_MODEL_PREPARE", "/bin/true");
      environment.insert("HM_HSTREAM_MODEL_INFER", "/bin/true");
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
      if (completed_receipt) {
        window.pause_button_->click();
        require(
            waitFor([&] { return window.pipeline_paused_ && window.pipeline_cooperative_pause_; }),
            "Cooperative pause is acknowledged by the backend");
        window.pipeline_process_->write("@test-ping\n");
        require(
            waitFor([&] { return window.log_->toPlainText().contains("BACKEND_PONG"); }),
            "Cooperative pause keeps the backend process responsive");
        require(
            window.scoreboard_select_button_->isEnabled(), "Cooperative HockeyMON pause permits scoreboard selection");
        window.active_hockeymon_program_ = false;
        window.pipeline_cooperative_pause_ = false;
        window.updateScoreboardControls();
        require(
            !window.scoreboard_select_button_->isEnabled(),
            "Native process pause still requires resume before selection");
        window.active_hockeymon_program_ = true;
        window.pipeline_cooperative_pause_ = true;
        window.pause_button_->click();
        require(
            waitFor([&] { return !window.pipeline_paused_ && window.pending_process_resume_generation_ == 0; }),
            "Cooperative resume is acknowledged by the backend");
        const QString pinned = window.backend_preview_directory_->filePath("pinned-scoreboard.png");
        QImage source(640, 360, QImage::Format_RGB32);
        source.fill(Qt::magenta);
        require(source.save(pinned), "Write private source canvas");
        writeFile(window.backend_preview_directory_->filePath("spec.json"), "{\"session_id\":\"scoreboard-session\"}");
        const QString url = "http://127.0.0.1:1/?token=" + QString(64, 'a');
        const QJsonObject descriptor{
            {"version", 1},
            {"game_id", "game"},
            {"session_id", "scoreboard-session"},
            {"url", url},
            {"image_path", pinned},
            {"points", QJsonArray{}},
            {"scope", "run"}};
        writeFile(
            window.backend_preview_directory_->filePath("scoreboard-selection.json"),
            QJsonDocument(descriptor).toJson());
        window.handleScoreboardSelectorOutput("Scoreboard selector: " + url);
        require(
            window.scoreboard_selection_dialog_ && window.scoreboard_selection_dialog_->loadError().isEmpty(),
            "Shared scoreboard loads its private canvas without saved game/s.png");
        auto* canvas = static_cast<ScoreboardSelectionCanvas*>(
            window.scoreboard_selection_dialog_->findChild<QWidget*>("scoreboardSelectionCanvas"));
        require(canvas->imageSize() == QSize(640, 360), "Shared selector shows private source dimensions");
        require(
            window.scoreboard_selection_dialog_->findChild<QPushButton*>("scoreboardSaveButton")->text() ==
                "Apply to This Run",
            "A source override exposes run-local selection scope");
        window.scoreboard_selection_dialog_->closeAfterBackendCompletion();
        require(waitFor([&] { return window.scoreboard_selection_dialog_ == nullptr; }), "Close fixture selector");
        window.scoreboard_selector_url_.clear();
        window.scoreboard_reselect_waiting_ = false;
      }
      window.preview_tabs_->setCurrentIndex(1);
      require(
          waitFor([&] {
            return window.stitched_render_target_->isVisible() && color(window.stitched_render_target_).green() > 200;
          }),
          "Tab switching selects and paints the stitched producer frame");
      for (int camera = 0; camera < 2; ++camera) {
        window.preview_tabs_->setCurrentIndex(2 + camera);
        require(
            waitFor([&] {
              auto* target = window.camera_preview_render_targets_[camera];
              const auto pixel = color(target);
              return target->isVisible() && (camera == 0 ? pixel.blue() > 200 : pixel.red() > 200);
            }),
            "Camera tabs consume the actual Python Camera 1/2 filenames");
        require(window.camera_preview_notices_[camera]->isHidden(), "A source frame clears its waiting placeholder");
      }
      window.preview_tabs_->setCurrentIndex(1);
      if (completed_receipt)
        window.stopPipeline();
      else
        window.pipeline_process_->write("@test-finish\n");
      require(
          waitFor([&] { return window.pipeline_process_->state() == QProcess::NotRunning; }),
          "Backend process must finish");
      require(window.backend_publication_complete_ == completed_receipt, "Completion requires a backend receipt");
      require(
          window.playback_progress_state_ ==
              (completed_receipt ? HStreamWindow::PlaybackProgressState::kStopped
                                 : HStreamWindow::PlaybackProgressState::kError),
          "Missing receipt must make a successful exit visibly fail");
      require(!window.preview_layout_compacted_, "Finishing restores the ordinary preview layout");
      require(
          window.stitched_render_target_->isVisible() && color(window.stitched_render_target_).green() > 200,
          "Final preview remains visible after success and failure");
    }
  }

  static void check(HStreamWindow& window, const QString& game_directory) {
    checkResolvedLaunch(window);
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
    const QByteArray pair_config =
        "game:\n  videos:\n    left: [left.mp4]\n    right: [right.mp4]\n"
        "hstream_ui:\n  stitching_calibration:\n    status: complete\n";
    writeFile(config_path, pair_config);
    const QString initial_path = QDir(game_directory).filePath("initial.json");
    const QString source_path = QDir(game_directory).filePath("prestitched.mp4");
    writeFile(source_path, "recording");
    QJsonObject initial{
        {"version", 1},
        {"module", "hmlib.cli.hmtrack"},
        {"game_id", "game"},
        {"cwd", game_directory},
        {"arguments", QJsonArray{"--input-video", source_path}}};
    window.backend_.initial_launch = initial_path;
    writeFile(initial_path, QJsonDocument(initial).toJson());
    window.startPipeline();
    require(
        window.pipeline_process_->state() == QProcess::NotRunning &&
            window.log_->toPlainText().contains("sources were not resolved for this launch"),
        "Legacy initial envelopes fail before any cleanup or calibration admission");
    QJsonObject ownership{
        {"version", 1},
        {"game_id", "game"},
        {"cwd", game_directory},
        {"arguments", initial.value("arguments")},
        {"sources", QJsonObject{{"source", QJsonArray{source_path}}}}};
    initial.insert("source_ownership", ownership);
    writeFile(initial_path, QJsonDocument(initial).toJson());
    require(window.resolveInitialProgramSources(), "Resolved initial source ownership is admitted");
    bool initial_calibration_required = true;
    require(
        window.prepareStitchingCalibrationRun(
            "/must/not/clean-saved-pair", game_directory, {}, &initial_calibration_required) &&
            !initial_calibration_required,
        "Initial single-source override bypasses saved pair cleanup");
    QFile protected_config(config_path);
    require(protected_config.open(QIODevice::ReadOnly), "Read protected pair config");
    require(protected_config.readAll() == pair_config, "Initial source admission leaves saved pair calibration intact");
    protected_config.close();
    initial.insert("arguments", QJsonArray{"--input-video", "changed-after-resolution.mp4"});
    writeFile(initial_path, QJsonDocument(initial).toJson());
    require(!window.resolveInitialProgramSources(), "Changed initial arguments invalidate resolved source ownership");
    window.backend_.initial_launch.clear();
    window.initial_program_requires_stitching_.reset();
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
    environment.insert("HM_HSTREAM_MODEL_PREPARE", "/bin/true");
    environment.insert("HM_HSTREAM_MODEL_INFER", "/bin/true");
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
    if (const QString game = qEnvironmentVariable("HM_UI_E2E_GAME"); !game.isEmpty()) {
      QApplication app(argc, argv);
      HStreamWindow window(
          nullptr, {"hockeymon", qEnvironmentVariable("HM_CLI_BIN"), qEnvironmentVariable("HM_UI_E2E_INITIAL_LAUNCH")});
      try {
        HStreamWindowTestAccess::checkRealBackend(window, game);
      } catch (...) {
        // Never tear down QWidget/QProcess state while a real worker remains
        // active, including when an integration assertion times out.
        HStreamWindowTestAccess::stopRealBackend(window);
        throw;
      }
      return 0;
    }
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
for name in ('preview-final.jpg', 'preview-stitched.jpg', 'preview-camera-1.jpg', 'preview-camera-2.jpg'):
    shutil.copyfile(pathlib.Path(os.environ['TEST_FRAMES']) / name, session / name)
print('Pipeline running', flush=True)
print('HSTREAM_PREVIEW_RUNTIME status=ready channel=program generation=1', flush=True)
print('HSTREAM_PROGRESS instance=aggregate stage=0 instances=1 generation=0 processed_ns=1000000000 fps=10 fps_avg=10 speed_x=1', flush=True)
for line in sys.stdin:
    fields = line.strip().split()
    if fields[0] == '@set-preview-active':
        channel, generation = fields[1:]
        status = 'deactivated' if channel == 'none' else 'activated'
        print(f'HSTREAM_PREVIEW channel={channel} status={status} generation={generation} message=shared preview', flush=True)
        if channel != 'none':
            print(f'HSTREAM_PREVIEW channel={channel} status=ready generation={generation} message=first frame resolution=320x180', flush=True)
    elif fields[0] == '@process-pause':
        print(f'HSTREAM_PROCESS_PAUSE status=prepared generation={fields[1]} mode=cooperative', flush=True)
    elif fields[0] == '@process-resume':
        print(f'HSTREAM_PROCESS_PAUSE status=resumed generation={fields[1]}', flush=True)
    elif fields[0] == '@reset-progress-rate':
        print(f'HSTREAM_PROGRESS status=reset generation={fields[1]}', flush=True)
    elif fields[0] == '@test-ping':
        print('BACKEND_PONG', flush=True)
    elif fields[0] in ('@test-finish', 'q'):
        if os.environ['TEST_RECEIPT'] == '1':
            print('HM_BACKEND_PUBLICATION ' + json.dumps({'version': 1, 'game_id': 'game', 'status': 'complete', 'artifacts': []}), flush=True)
        break
)PY");
    QImage frame(320, 180, QImage::Format_RGB32);
    frame.fill(Qt::red);
    require(frame.save(root.filePath("preview-final.jpg")), "Write Program frame fixture");
    frame.fill(Qt::green);
    require(frame.save(root.filePath("preview-stitched.jpg")), "Write stitched frame fixture");
    frame.fill(Qt::blue);
    require(frame.save(root.filePath("preview-camera-1.jpg")), "Write first source frame fixture");
    frame.fill(Qt::yellow);
    require(frame.save(root.filePath("preview-camera-2.jpg")), "Write second source frame fixture");
    HStreamWindowTestAccess::checkLifecycle(window, runner, root.path());
    std::cout << "Shared backend runner, publication, preview and detector isolation checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
