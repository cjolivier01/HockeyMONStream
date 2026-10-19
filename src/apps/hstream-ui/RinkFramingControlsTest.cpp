#include "src/apps/hstream-ui/RinkFramingControls.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <atomic>
#include <iostream>

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  if (argc == 4 && QString(argv[1]) == "--inspect-game") {
    RinkFramingControls controls;
    controls.resize(680, 380);
    controls.show();
    controls.setGameDirectory(QString::fromLocal8Bit(argv[2]));
    auto* enable = controls.findChild<QCheckBox*>("rinkFramingEnable");
    auto* status = controls.findChild<QLabel*>("rinkFramingStatus");
    QElapsedTimer timer;
    timer.start();
    while (!enable->isEnabled() && timer.elapsed() < 30000)
      QTest::qWait(20);
    if (!enable->isEnabled()) {
      std::cerr << status->text().toStdString() << '\n';
      return 1;
    }
    enable->setChecked(true);
    const QString prefix = QString::fromLocal8Bit(argv[3]);
    QTest::qWait(20);
    if (!controls.grab().save(prefix + "-ready.png"))
      return 1;
    QTemporaryDir missing_map;
    controls.setGameDirectory(missing_map.path());
    timer.restart();
    while (status->text().contains("Checking") && timer.elapsed() < 30000)
      QTest::qWait(20);
    if (enable->isEnabled() || controls.validateForRun().isEmpty() || status->text().contains("Checking") ||
        !controls.grab().save(prefix + "-blocked.png"))
      return 1;
    controls.findChild<QPushButton*>("rinkFramingTurnOff")->click();
    QTest::qWait(20);
    return !enable->isChecked() && controls.validateForRun().isEmpty() && controls.grab().save(prefix + "-disabled.png")
        ? 0
        : 1;
  }
  QTemporaryDir map_directory;
  std::atomic<int> checks{0};
  RinkFramingControls controls(nullptr, [&](const QString& path, const std::function<bool()>& cancelled) {
    ++checks;
    QFile file(path + "/rink-map.yaml");
    if (!file.open(QIODevice::ReadOnly))
      return absl::FailedPreconditionError("Save a reviewed rink map");
    return file.readAll() == "ready" ? absl::OkStatus() : absl::FailedPreconditionError("The rink map is partial");
  });
  auto wait = [](const std::function<bool()>& condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 5000)
      QTest::qWait(10);
    return condition();
  };
  auto* enable = controls.findChild<QCheckBox*>("rinkFramingEnable");
  auto* anticipate = controls.findChild<QCheckBox*>("rinkFramingAnticipate");
  auto* goals = controls.findChild<QCheckBox*>("rinkFramingGoals");
  auto* zoom = controls.findChild<QCheckBox*>("rinkFramingZoom");
  const QString prefix = "--options=pipeline.ds-playtracker.private-properties.";
  bool ok = !enable->isChecked() && anticipate->isChecked() && !anticipate->isEnabled() && !goals->isChecked() &&
      !zoom->isChecked() && !controls.isDirty() && controls.arguments().contains(prefix + "rink-camera-enable=0");
  ok &= !enable->isEnabled();
  controls.setGameDirectory(map_directory.path());
  ok &= wait([&] { return controls.findChild<QLabel*>("rinkFramingStatus")->text().contains("Save a reviewed"); });
  enable->setChecked(true);
  ok &= !controls.validateForRun().isEmpty() && enable->isChecked() && !enable->isEnabled();
  controls.findChild<QPushButton*>("rinkFramingTurnOff")->click();
  ok &= !enable->isChecked() && controls.validateForRun().isEmpty();
  auto saveMap = [&](const QByteArray& value) {
    QFile file(map_directory.filePath("rink-map.yaml"));
    return file.open(QIODevice::WriteOnly) && file.write(value) == value.size();
  };
  ok &= saveMap("ready") && wait([&] { return enable->isEnabled(); });
  YAML::Node empty(YAML::NodeType::Map), user, game;
  user["pipeline"]["ds-playtracker"]["private-properties"]["rink-camera-enable"] = true;
  game["pipeline"]["ds-playtracker"]["private-properties"]["rink-camera-enable"] = 0;
  controls.loadConfig(empty, user, game);
  ok &= !enable->isChecked() && !controls.isDirty();
  enable->setChecked(true);
  ok &= anticipate->isEnabled() && anticipate->isChecked() && !goals->isChecked() && !zoom->isChecked() &&
      controls.isDirty() && controls.arguments().contains(prefix + "rink-camera-enable=1") &&
      controls.arguments().contains(prefix + "rink-camera-frame-goals=0") &&
      controls.arguments().contains(prefix + "rink-camera-adaptive-zoom=0");
  controls.applyChanges(game);
  ok &= game["pipeline"]["ds-playtracker"]["private-properties"]["rink-camera-enable"].as<int>() == 1;
  controls.loadConfig(empty, user, game);
  ok &= !controls.isDirty() && enable->isChecked();
  anticipate->setChecked(false);
  ok &= !controls.validateForRun().isEmpty();
  anticipate->setChecked(true);
  ok &= controls.validateForRun().isEmpty();
  controls.setCheckingSuspended(true);
  controls.setCheckingSuspended(false);
  ok &= enable->isEnabled() && !controls.isCheckingForRun() && controls.validateForRun().isEmpty();
  ok &= !controls.validateSavedSettings(YAML::Node(YAML::NodeType::Map)).isEmpty();
  auto settings = YAML::Load("hstream_ui: {stitching_calibration: {status: complete}}");
  ok &= controls.validateSavedSettings(settings).isEmpty();
  settings["hstream_ui"]["stitching_calibration"]["stale_from"] = "canvas";
  ok &= !controls.validateSavedSettings(settings).isEmpty();
  // The authoritative identity can change independently of every other file.
  {
    QFile identity(map_directory.filePath("stitching_generation_id"));
    ok &= identity.open(QIODevice::WriteOnly) && identity.write("new identity") > 0;
  }
  ok &= !controls.validateForRun().isEmpty() && !enable->isEnabled();
  ok &= wait([&] { return enable->isEnabled(); });
  // Play rechecks file identity even before the filesystem notification arrives.
  ok &= saveMap("partial");
  ok &= !controls.validateForRun().isEmpty() && !enable->isEnabled() && enable->isChecked();
  ok &= wait([&] { return controls.findChild<QLabel*>("rinkFramingStatus")->text().contains("partial"); });
  const int checked = checks.load();
  {
    QFile unrelated(map_directory.filePath("output.mp4"));
    unrelated.open(QIODevice::WriteOnly);
    unrelated.write("output");
  }
  QTest::qWait(250);
  ok &= checks == checked;
  controls.setCheckingSuspended(true);
  ok &= saveMap("ready");
  QTest::qWait(250);
  ok &= checks == checked && !enable->isEnabled();
  controls.setCheckingSuspended(false);
  ok &= saveMap("ready") && wait([&] { return enable->isEnabled(); });
  controls.setGameDirectory(map_directory.filePath("missing"));
  ok &= !enable->isEnabled() && !controls.validateForRun().isEmpty();
  controls.resetToDefaults();
  ok &= controls.isDirty() && !enable->isChecked();
  if (!ok)
    std::cerr << "Rink framing defaults, precedence, persistence or exported arguments differ\n";
  return ok ? 0 : 1;
}
