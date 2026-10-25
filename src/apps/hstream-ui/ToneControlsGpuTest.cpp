#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtGui/QWheelEvent>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <functional>
#include <iostream>
#include <stdexcept>
#include "src/apps/hstream-ui/HStreamWindow.h"

namespace {
void Check(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
void Wait(const std::function<bool()>& ready, int timeout, const char* message) {
  QElapsedTimer clock;
  clock.start();
  while (!ready() && clock.elapsed() < timeout)
    QTest::qWait(20);
  Check(ready(), message);
}
} // namespace
struct HStreamWindowTestAccess {
  HStreamWindow& w;
  QFile wire;
  QJsonObject session;
  std::map<int, QJsonObject> replies;
  QStringList delayed;
  bool delay_ack{false};
  bool saw_positive_output{false};
  QMetaObject::Connection stdout_connection, stderr_connection;
  int sequence{0};
  QString root, artifacts;
  HStreamWindowTestAccess(HStreamWindow& window, const QString& native, const QString& output)
      : w(window), wire(output + "/native.log"), root(native), artifacts(output) {
    Check(wire.open(QIODevice::WriteOnly), "Cannot open native transcript");
    w.development_runtime_root_ = root;
    w.development_bazel_bin_ = root + "/bazel-bin";
    w.development_pipeline_runner_ = root + "/bazel-bin/src/apps/hstream-cli/hstream-cli";
    QObject::disconnect(w.pipeline_process_, SIGNAL(readyReadStandardOutput()), &w, nullptr);
    QObject::disconnect(w.pipeline_process_, SIGNAL(readyReadStandardError()), &w, nullptr);
    stdout_connection =
        QObject::connect(w.pipeline_process_, &QProcess::readyReadStandardOutput, &w, [this] { Pump(); });
    stderr_connection =
        QObject::connect(w.pipeline_process_, &QProcess::readyReadStandardError, &w, [this] { Pump(); });
  }
  ~HStreamWindowTestAccess() {
    if (w.pipeline_process_->state() != QProcess::NotRunning) {
      w.stopPipeline();
      try {
        Wait([this] { return w.pipeline_process_->state() == QProcess::NotRunning; }, 30000, "Stop timed out");
      } catch (...) {
      }
    }
    QObject::disconnect(stdout_connection);
    QObject::disconnect(stderr_connection);
  }
  void Pump() {
    auto drain = [this](QByteArray bytes, QString* buffer, bool error) {
      wire.write(bytes);
      wire.flush();
      *buffer += QString::fromLocal8Bit(bytes);
      while (buffer->contains('\n')) {
        const int end = buffer->indexOf('\n');
        const QString line = buffer->left(end).trimmed();
        buffer->remove(0, end + 1);
        static const QRegularExpression fps(R"(\*\*PERF:\s+([0-9]+(?:\.[0-9]+)?))");
        const auto match = fps.match(line);
        if (match.hasMatch() && match.captured(1).toDouble() > 0)
          saw_positive_output = true;
        if (line.startsWith("HSTREAM_PIPELINE_INSPECTOR ")) {
          const auto event = QJsonDocument::fromJson(line.mid(27).toUtf8()).object();
          if (event["kind"] == "session") {
            session = event;
            std::cout << "SESSION " << QJsonDocument(event).toJson(QJsonDocument::Compact).constData() << std::endl;
          }
        }
        if (line.startsWith("HSTREAM_TONE_CONTROLS ")) {
          const auto event = QJsonDocument::fromJson(line.mid(22).toUtf8()).object();
          replies[event["requestId"].toInt()] = event;
          std::cout << "READBACK " << QJsonDocument(event).toJson(QJsonDocument::Compact).constData() << std::endl;
        }
        if (delay_ack && line.startsWith("runtime property "))
          delayed.push_back(line);
        else
          w.handlePipelineOutputLine(line, error);
      }
    };
    drain(w.pipeline_process_->readAllStandardError(), &w.pipeline_stderr_buffer_, true);
    drain(w.pipeline_process_->readAllStandardOutput(), &w.pipeline_stdout_buffer_, false);
  }
  QJsonObject Query() {
    const int id = ++sequence;
    const QByteArray command = QString("@query-tone-controls %1 %2 %3\n")
                                   .arg(id)
                                   .arg(session["stage"].toInt())
                                   .arg(session["generation"].toInt())
                                   .toUtf8();
    Check(w.pipeline_process_->write(command) == command.size(), "Cannot send native readback query");
    Wait([&] { return replies.count(id); }, 10000, "Native readback query timed out");
    return replies.at(id);
  }
  void Values(int shadows, int black, int exposure, bool high) {
    const auto reply = Query();
    Check(reply["status"] == "ok", "Native tone readback rejected: " + QJsonDocument(reply).toJson().toStdString());
    Check(
        reply["element"] == (high ? "hmstitcher0" : "playcropper0") && reply["highBitDepth"].toBool() == high,
        "Wrong actual precision/owner");
    std::map<QString, int> values;
    for (const auto& value : reply["controls"].toArray())
      values[value.toObject()["id"].toString()] = value.toObject()["value"].toInt();
    Check(
        values["Bring_Up_Shadows"] == shadows && values["Lift_Shadow_Black_Point"] == black &&
            values["Exposure_x100"] == exposure,
        "Actual native readback disagrees with Qt values");
  }
  void Tab(const QString& stage, const QString& controls) {
    for (int i = 0; i < w.preview_tabs_->count(); ++i)
      if (w.preview_tabs_->tabText(i) == stage)
        w.preview_tabs_->setCurrentIndex(i);
    for (auto* tabs : w.findChildren<QTabWidget*>())
      for (int i = 0; i < tabs->count(); ++i)
        if (tabs->tabText(i) == controls)
          tabs->setCurrentIndex(i);
    QTest::qWait(100);
  }
  void Run(bool high) {
    const QString game = qEnvironmentVariable("TONE_GAME_ID");
    Check(!game.isEmpty(), "TONE_GAME_ID must name a disposable copied fixture");
    Check(
        QFileInfo::exists(qEnvironmentVariable("HM_GAME_DIR") + "/" + game + "/.hstream-tone-gpu-fixture"),
        "Run this test through scripts/test_hstream_tone_controls_gpu.py");
    w.selectGame(game);
    for (auto* box : w.findChildren<QCheckBox*>())
      if (box->objectName().startsWith("outputToggle_"))
        box->setChecked(false);
    w.render_video_toggle_->setChecked(false);
    w.drivegpt_csv_toggle_->setChecked(false);
    w.gpu_memory_profile_combo_->setCurrentIndex(w.gpu_memory_profile_combo_->findData("low"));
    w.setHighBitDepthMode(high ? "1" : "0");
    w.camera_sliders_.at("Bring_Up_Shadows")->setValue(0);
    w.camera_checkboxes_.at("Lift_Shadow_Black_Point")->setChecked(false);
    w.camera_sliders_.at("Exposure_x100")->setValue(0);
    Tab("Program", "Color");
    w.startPipeline();
    Wait(
        [&] { return session["generation"].toInt() > 0 && w.active_run_high_bit_depth_resolved_; },
        180000,
        "Qt did not reach a native session with resolved precision");
    Wait(
        [&] { return w.pipelineStateText() == "PLAYING" && saw_positive_output; },
        180000,
        "Qt did not report processing FPS");
    Values(0, 0, 0, high);
    auto* shadows = w.camera_sliders_.at("Bring_Up_Shadows");
    auto* exposure = w.camera_sliders_.at("Exposure_x100");
    auto* black = w.camera_checkboxes_.at("Lift_Shadow_Black_Point");
    Check(!black->isEnabled(), "Black point was enabled at zero shadows");
    delay_ack = true;
    shadows->setValue(34);
    shadows->setValue(35);
    black->setChecked(true);
    exposure->setValue(61);
    Check(w.runtime_control_batches_.empty(), "Qt bypassed debounce");
    Wait([&] { return delayed.size() >= 3; }, 10000, "GPU tone setters did not acknowledge initial batch");
    Check(
        w.runtime_control_batches_.size() == 1,
        "Qt did not retain one pending batch while native ACK delivery was delayed");
    Values(35, 1, 61, high);
    exposure->setValue(72);
    QTest::qWait(160);
    Check(delayed.size() == 3, "Qt sent another mutation before pending batch completed");
    delay_ack = false;
    const auto acknowledgments = delayed;
    delayed.clear();
    for (const auto& line : acknowledgments)
      w.handlePipelineOutputLine(line, false);
    Wait(
        [&] { return w.runtime_control_batches_.empty() && w.scheduled_playcropper_controls_.empty(); },
        10000,
        "Queued Qt edit did not complete");
    Values(35, 1, 72, high);
    Check(
        w.stitched_color_sliders_.at("Bring_Up_Shadows")->value() == 35 &&
            w.stitched_color_sliders_.at("Exposure_x100")->value() == 72 &&
            w.stitched_color_checkboxes_.at("Lift_Shadow_Black_Point")->isChecked(),
        "Mirrored dock diverged");
    shadows->setFocus();
    const QPointF position(shadows->rect().center());
    QWheelEvent wheel(
        position,
        shadows->mapToGlobal(position.toPoint()),
        QPoint(),
        QPoint(0, 120),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false);
    QApplication::sendEvent(shadows, &wheel);
    Check(shadows->value() == 35, "Wheel changed focused tone value");
    Check(w.grab().save(artifacts + "/qt-program.png"), "Cannot save Program screenshot");
    Tab("Stitched", "Color & Precision");
    auto* mirrored = w.stitched_color_sliders_.at("Exposure_x100");
    mirrored->setFocus();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QApplication::sendEvent(mirrored, &press);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Right, Qt::NoModifier);
    QApplication::sendEvent(mirrored, &release);
    Wait(
        [&] { return w.runtime_control_batches_.empty() && w.scheduled_playcropper_controls_.empty(); },
        10000,
        "Mirrored keyboard edit did not complete");
    Values(35, 1, 73, high);
    w.stitched_color_sliders_.at("Bring_Up_Shadows")->setValue(0);
    Wait(
        [&] { return w.runtime_control_batches_.empty() && w.scheduled_playcropper_controls_.empty(); },
        10000,
        "Shadow disable did not complete");
    Values(0, 1, 73, high);
    Check(
        black->isChecked() && !black->isEnabled() &&
            w.stitched_color_checkboxes_.at("Lift_Shadow_Black_Point")->isChecked() &&
            !w.stitched_color_checkboxes_.at("Lift_Shadow_Black_Point")->isEnabled(),
        "Disabled black-point value was lost");
    Check(w.grab().save(artifacts + "/qt-stitched.png"), "Cannot save Stitched screenshot");
    auto* precision = w.stitched_color_checkboxes_.at("Use_10_Bit_Grading");
    Check(precision->isEnabled(), "Next-run precision control is disabled during playback");
    const QString next_mode = high ? "0" : "1";
    for (int attempt = 0; attempt < 3 && w.highBitDepthMode() != next_mode; ++attempt)
      precision->click();
    Check(w.highBitDepthMode() == next_mode, "Next-run precision checkbox did not cycle");
    Values(0, 1, 73, high);
    Check(w.savePreset(), "Qt Save Preset failed during live run");
    Values(0, 1, 73, high);
    w.stopPipeline();
    Wait([&] { return w.pipeline_process_->state() == QProcess::NotRunning; }, 30000, "Qt stop did not finish");
    Check(
        w.pipeline_process_->exitStatus() == QProcess::NormalExit && w.pipeline_process_->exitCode() == 0,
        "Native process did not stop successfully");
    std::cout << "PASS Qt GPU tone acceptance " << (high ? "high-bit" : "standard") << std::endl;
  }
};
int main(int argc, char** argv) {
  const QString mode = qEnvironmentVariable("TONE_HIGH_BIT");
  if (mode != "0" && mode != "1") {
    std::cerr << "Use scripts/test_hstream_tone_controls_gpu.py with an explicit precision mode\n";
    return 2;
  }
  QApplication app(argc, argv);
  const QString root = qEnvironmentVariable("TONE_NATIVE_ROOT"), artifacts = qEnvironmentVariable("TONE_ARTIFACTS");
  if (root.isEmpty() || artifacts.isEmpty() || !QDir(artifacts).exists() ||
      !QFileInfo(root + "/bazel-bin/src/apps/hstream-cli/hstream-cli").isExecutable()) {
    std::cerr << "The native runner and artifact directory must exist\n";
    return 2;
  }
  hm::ui::BackendOptions backend;
  backend.runner = root + "/bazel-bin/src/apps/hstream-cli/hstream-cli";
  HStreamWindow window(nullptr, backend);
  window.resize(1500, 1000);
  window.show();
  QTimer dialog_watch;
  QObject::connect(&dialog_watch, &QTimer::timeout, [&] {
    for (auto* widget : QApplication::topLevelWidgets())
      if (auto* dialog = qobject_cast<QMessageBox*>(widget); dialog && dialog->isVisible()) {
        std::cerr << "UNEXPECTED DIALOG " << dialog->text().toStdString() << std::endl;
        dialog->reject();
      }
  });
  dialog_watch.start(200);
  try {
    HStreamWindowTestAccess test(window, root, artifacts);
    test.Run(qEnvironmentVariableIntValue("TONE_HIGH_BIT") != 0);
    return 0;
  } catch (const std::exception& error) {
    window.grab().save(artifacts + "/failure.png");
    QFile log(artifacts + "/qt.log");
    if (log.open(QIODevice::WriteOnly))
      log.write(window.completeLogText().toUtf8());
    std::cerr << error.what() << '\n';
    return 1;
  }
}
