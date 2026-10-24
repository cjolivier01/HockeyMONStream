#include "Int8PreparationDialog.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>

#include <functional>
#include <iostream>

namespace {
bool waitFor(const std::function<bool()>& condition) {
  QElapsedTimer timer;
  timer.start();
  while (!condition() && timer.elapsed() < 10000)
    QTest::qWait(10);
  return condition();
}
bool write(const QString& path, const QByteArray& data) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
bool check(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}
} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir root;
  if (!root.isValid())
    return 1;
  const QString script = root.filePath("prepare.sh");
  const QString engine = root.filePath("detector_Test_GPU_int8.engine");
  const QString manifest = root.filePath("manifest.json");
  const QString assets = root.filePath("src/apps/hstream-assets/hstream-assets");
  QDir().mkpath(QFileInfo(assets).absolutePath());
  if (!write(engine, "engine") || !write(manifest, "{}") || !write(assets, "#!/bin/sh\nexit 0\n") ||
      !QFile::setPermissions(assets, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
    return 1;
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("HSTREAM_INT8_PREPARER", script);
  environment.insert("HSTREAM_INT8_BUILDER", assets);
  environment.insert("HSTREAM_INT8_PYTHON", "/bin/sh");
  hm::ui::Int8PreparationRequest request{
      assets, root.path(), root.path(), "app.yaml", "detector.yaml", "game", environment};
  const QByteArray ready = "HSTREAM_INT8_READY " +
      QJsonDocument(QJsonObject{{"engine", engine}, {"manifest", manifest}}).toJson(QJsonDocument::Compact);
  bool ok = true;
  for (int code : {0, 1}) {
    // A result may span several stdout chunks. Even a valid ready message must
    // never select an artifact after a nonzero exit.
    const auto split = ready.size() / 2;
    if (!write(
            script,
            "printf '%s' '" + ready.left(split) + "'\nsleep 0.1\nprintf '%s\\n' '" + ready.mid(split) + "'\nexit " +
                QByteArray::number(code) + "\n"))
      return 1;
    hm::ui::Int8PreparationDialog dialog(request);
    bool accepted = false;
    QObject::connect(&dialog, &QDialog::accepted, [&] { accepted = true; });
    auto* button = dialog.findChild<QPushButton*>("int8PrepareButton");
    auto* status = dialog.findChild<QLabel*>("int8PreparationStatus");
    button->click();
    ok &= check(
        waitFor([&] { return accepted || status->text().startsWith("Preparation failed"); }),
        "preparation did not finish");
    ok &= check(accepted == (code == 0), "selection must require successful process exit");
    ok &= check(
        code == 0 ? dialog.engine() == engine : dialog.engine().isEmpty(), "failed result leaked a selected engine");
  }
  if (!write(script, "exec sleep 60\n"))
    return 1;
  {
    hm::ui::Int8PreparationDialog dialog(request);
    bool rejected = false;
    QObject::connect(&dialog, &QDialog::rejected, [&] { rejected = true; });
    dialog.findChild<QPushButton*>("int8PrepareButton")->click();
    QTest::qWait(100);
    dialog.reject();
    dialog.reject();
    ok &= check(
        waitFor([&] { return rejected; }) && dialog.engine().isEmpty(),
        "cancel must stop preparation without selection");
  }
  QFile::remove(script);
  {
    hm::ui::Int8PreparationDialog dialog(request);
    dialog.findChild<QPushButton*>("int8PrepareButton")->click();
    ok &= check(
        dialog.findChild<QLabel*>("int8PreparationStatus")->text().contains("unavailable") && dialog.engine().isEmpty(),
        "missing tools must leave preparation inactive");
  }
  {
    // The shared desktop delegates Python preparation to hm-cli directly;
    // native TensorRT tools are neither required nor passed to that backend.
    const QString runner = root.filePath("hm-cli");
    const QString arguments = root.filePath("arguments.txt");
    if (!write(
            runner,
            "#!/bin/sh\nprintf '%s\\n' \"$@\" > '" + arguments.toUtf8() + "'\nprintf '%s\\n' '" + ready + "'\n") ||
        !QFile::setPermissions(runner, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
      return 1;
    auto python_request = request;
    python_request.runner = runner;
    python_request.hockeymon = true;
    python_request.detector_model = "yolo12l";
    hm::ui::Int8PreparationDialog dialog(python_request);
    bool accepted = false;
    QObject::connect(&dialog, &QDialog::accepted, [&] { accepted = true; });
    dialog.findChild<QPushButton*>("int8PrepareButton")->click();
    ok &= check(waitFor([&] { return accepted; }), "Python INT8 preparation must use the backend runner");
    QFile recorded(arguments);
    ok &= check(
        recorded.open(QIODevice::ReadOnly) &&
            recorded.readAll() == "--prepare-detector\n--game-id\ngame\n--samples\n64\n--detector-model\nyolo12l\n",
        "Python preparation arguments must match the backend protocol");
    ok &= check(
        dialog.engine() == engine && dialog.manifest() == manifest, "Python preparation must return its artifacts");
  }
  return ok ? 0 : 1;
}
