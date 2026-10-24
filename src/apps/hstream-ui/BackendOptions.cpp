#include "src/apps/hstream-ui/BackendOptions.h"

#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>

#include <stdexcept>

namespace hm::ui {
namespace {
void fail(const QString& message) {
  throw std::runtime_error(message.toStdString());
}
} // namespace

void BackendOptions::validate() const {
  if (name != "hstream" && name != "hockeymon")
    fail("Unknown UI backend: " + name);
  if (!runner.isEmpty()) {
    const QFileInfo info(runner);
    if (!info.isAbsolute() || !info.isFile() || !info.isExecutable())
      fail("--backend-runner must name an absolute executable file: " + runner);
  } else if (isHockeyMon()) {
    fail("The HockeyMON backend requires --backend-runner=/absolute/path/to/hm-cli");
  }
  if (!initial_launch.isEmpty()) {
    const QFileInfo info(initial_launch);
    if (!isHockeyMon() || !info.isAbsolute() || !info.isFile() || info.size() > 1024 * 1024)
      fail("--backend-launch requires a bounded HockeyMON launch document at an absolute path");
  }
}

void BackendOptions::validateHandshake(const QJsonObject& handshake) {
  if (handshake.value("protocol").toString() != "hstream-ui" || handshake.value("version").toDouble(-1) != 1 ||
      handshake.value("backend").toString() != "hockeymon" || handshake.value("preview").toString() != "jpeg-files")
    fail("HockeyMON runner does not implement hstream-ui protocol version 1 with jpeg-files previews");
}

void BackendOptions::verifyRunner() const {
  validate();
  if (!isHockeyMon())
    return;
  QProcess process;
  process.setProgram(runner);
  process.setArguments({"--ui-backend-info"});
  process.start();
  if (!process.waitForStarted(5000))
    fail("Could not launch HockeyMON runner: " + process.errorString());
  if (!process.waitForFinished(5000)) {
    process.kill();
    process.waitForFinished(5000);
    fail("HockeyMON backend handshake timed out");
  }
  const auto output = process.readAllStandardOutput();
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || output.size() > 65536)
    fail("HockeyMON backend handshake failed: " + QString::fromUtf8(process.readAllStandardError()).left(4096));
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(output, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    fail("HockeyMON backend handshake is not a JSON object");
  validateHandshake(document.object());
}

} // namespace hm::ui
