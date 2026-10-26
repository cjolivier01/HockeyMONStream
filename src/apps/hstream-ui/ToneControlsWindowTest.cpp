#include "src/apps/hstream-ui/HStreamWindow.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>

#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
QByteArray ReadCommand(QProcess& process) {
  QByteArray command;
  QElapsedTimer timer;
  timer.start();
  while (!command.contains('\n') && timer.elapsed() < 3000) {
    QTest::qWait(5);
    command += process.readAllStandardOutput();
  }
  Check(command.contains('\n'), "Timed out waiting for the debounced Qt command");
  return command;
}
void NoCommand(QProcess& process) {
  QTest::qWait(180);
  Check(process.readAllStandardOutput().isEmpty(), "Qt sent a command while the control should be blocked");
}
} // namespace

struct HStreamWindowTestAccess {
  // Echo only: exercise the actual Qt scheduler/wire without a video backend.
  struct Process {
    HStreamWindow& window;
    QProcess echo;
    QProcess* original;
    explicit Process(HStreamWindow& value) : window(value), original(value.pipeline_process_) {
      echo.start("/bin/cat", QStringList{});
      Check(echo.waitForStarted(), "Cannot start command echo fixture");
      window.pipeline_process_ = &echo;
    }
    ~Process() {
      window.pipeline_process_ = original;
      window.active_run_is_calibration_ = false;
      window.active_hockeymon_program_ = false;
      echo.closeWriteChannel();
      if (!echo.waitForFinished(1000)) {
        echo.kill();
        echo.waitForFinished();
      }
    }
  };

  static void Ack(HStreamWindow& window, const QByteArray& command) {
    const auto assignments = QString::fromUtf8(command).trimmed().mid(QString("@set-properties ").size()).split(';');
    for (const auto& assignment : assignments)
      window.handleRuntimeControlResponse("runtime property " + assignment);
  }

  static void Run(HStreamWindow& window) {
    window.selectGame("tone-fixture");
    auto* shadows = window.camera_sliders_.at("Bring_Up_Shadows");
    auto* exposure = window.camera_sliders_.at("Exposure_x100");
    auto* black = window.camera_checkboxes_.at("Lift_Shadow_Black_Point");
    Check(
        shadows->value() == 35 && exposure->value() == 61 && black->isChecked(),
        "Qt tone widgets ignored the resolved baseline/user overlay");
    Check(
        window.runtime_control_debounce_ms_ == 180 && window.runtimeControlAckTimeoutMs() == 4000 &&
            window.pipeline_stop_timeout_ms_ == 31000 && window.pipeline_kill_timeout_ms_ == 6000,
        "Qt runtime timers ignored the resolved baseline/user overlay");
    Check(shadows->minimum() == 0 && shadows->maximum() == 100, "Qt shadow bounds changed");
    Check(exposure->minimum() == 0 && exposure->maximum() == 130, "Qt exposure bounds changed");
    shadows->setValue(0);
    exposure->setValue(0);
    black->setChecked(false);
    Process process(window);
    for (bool hockeymon : {false, true}) {
      window.active_hockeymon_program_ = hockeymon;
      window.active_run_high_bit_depth_ = false;
      window.active_run_is_calibration_ = false;
      shadows->setValue(34);
      shadows->setValue(35);
      black->setChecked(true);
      exposure->setValue(61);
      Check(window.runtime_control_batches_.empty(), "Tone edits bypassed debounce");
      auto command = ReadCommand(process.echo);
      Check(
          command ==
              "@set-properties playcropper0 shadow-lift=35;playcropper0 shadow-lift-black-point=1;"
              "playcropper0 exposure=0.61\n",
          "Qt changed tone batch order, units, target or wire protocol");
      Check(
          window.runtime_control_batches_.size() == 1 && window.pending_runtime_controls_.size() == 3,
          "Tone batch must await all acknowledgments");
      exposure->setValue(72);
      NoCommand(process.echo);
      Ack(window, command);
      command = ReadCommand(process.echo);
      Check(command == "@set-properties playcropper0 exposure=0.72\n", "Queued tone edit was lost or replayed twice");
      window.handleRuntimeControlResponse("runtime command failed: plugin rejected playcropper0.exposure=0.72");
      Check(
          window.runtime_control_batches_.empty() && window.pending_runtime_controls_.empty(),
          "Rejected tone edit remains pending");
      Check(
          window.logText().contains("camera control Exposure_x100=72 apply=failed"),
          "Rejected tone edit was not reported as failed");
      window.pipeline_paused_ = true;
      exposure->setValue(73);
      NoCommand(process.echo);
      Check(window.runtime_control_batches_.empty(), "Paused tone draft started its acknowledgment timeout");
      window.pipeline_paused_ = false;
      window.pending_process_resume_generation_ = 17;
      window.flushScheduledRuntimeControls();
      NoCommand(process.echo);
      window.handleProcessPauseOutput("HSTREAM_PROCESS_PAUSE status=resumed generation=17");
      command = ReadCommand(process.echo);
      Check(command == "@set-properties playcropper0 exposure=0.73\n", "Confirmed Resume lost the paused tone draft");
      Ack(window, command);
      window.pending_playback_seek_generation_ = 9;
      shadows->setValue(44);
      NoCommand(process.echo);
      window.pending_playback_seek_generation_ = 0;
      window.flushScheduledRuntimeControls();
      command = ReadCommand(process.echo);
      Check(command == "@set-properties playcropper0 shadow-lift=44\n", "Seek-blocked tone edit was not retained");
      Ack(window, command);
      window.active_run_is_calibration_ = true;
      shadows->setValue(45);
      NoCommand(process.echo);
      window.active_run_high_bit_depth_ = true;
      shadows->setValue(46);
      black->setChecked(false);
      exposure->setValue(1);
      command = ReadCommand(process.echo);
      Check(
          command ==
              "@set-properties hmstitcher0 shadow-lift=46;hmstitcher0 shadow-lift-black-point=0;"
              "hmstitcher0 exposure=0.01\n",
          "High-bit calibration tone owner or units changed");
      Ack(window, command);
      Check(
          window.runtime_control_batches_.empty() && window.pending_runtime_controls_.empty(),
          "Successful tone batch remains pending");
      Check(
          window.stitched_color_sliders_.at("Bring_Up_Shadows")->value() == 46 &&
              window.stitched_color_sliders_.at("Exposure_x100")->value() == 1 &&
              !window.stitched_color_checkboxes_.at("Lift_Shadow_Black_Point")->isChecked(),
          "Program and Stitched tone controls diverged");
    }
    window.setHighBitDepthMode("0");
    window.resetCameraControls();
    Check(window.highBitDepthMode() == "0", "Live Reset changed next-run precision");
    Check(
        window.stitched_color_checkboxes_.at("Use_10_Bit_Grading")->checkState() == Qt::Unchecked,
        "Live Reset changed the mirrored next-run precision");
  }
};

int main(int argc, char** argv) {
  try {
    QTemporaryDir preferences;
    QTemporaryDir games;
    Check(preferences.isValid() && games.isValid(), "Cannot create isolated fixture directories");
    qputenv("XDG_CONFIG_HOME", preferences.path().toLocal8Bit());
    qputenv("HM_GAME_DIR", games.path().toLocal8Bit());
    qputenv("HOME", preferences.path().toLocal8Bit());
    Check(QDir().mkpath(preferences.filePath(".config/hstream-ui")), "Cannot create the private defaults directory");
    QFile overlay(preferences.filePath(".config/hstream-ui/defaults.yaml"));
    Check(overlay.open(QIODevice::WriteOnly), "Cannot write the private defaults overlay");
    overlay.write(
        "hstream_ui:\n  camera_controls:\n    Bring_Up_Shadows: 35\n"
        "    Lift_Shadow_Black_Point: 1\n    Exposure_x100: 61\n"
        "  runtime_controls:\n    debounce_ms: 180\n    ack_timeout_ms: 4000\n"
        "    stop_timeout_ms: 31000\n    kill_timeout_ms: 6000\n");
    overlay.close();
    Check(QDir().mkpath(games.filePath("tone-fixture")), "Cannot create the fixture game");
    QApplication app(argc, argv);
    HStreamWindow window;
    HStreamWindowTestAccess::Run(window);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
