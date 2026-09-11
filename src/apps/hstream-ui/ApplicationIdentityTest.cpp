#include "src/apps/hstream-ui/ApplicationIdentity.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtWidgets/QApplication>

#include <iostream>

namespace {
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}

QByteArray read_file(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool write_file(const QString& path, const QByteArray& contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}
} // namespace

int main(int argc, char** argv) {
  QTemporaryDir root;
  if (!root.isValid())
    return 1;
  const QString data_home = root.filePath("user-data");
  const QString system_data = root.filePath("system-data");
  qputenv("XDG_DATA_HOME", data_home.toUtf8());
  qputenv("XDG_DATA_DIRS", system_data.toUtf8());
  hm::ui_internal::configure_application_identity();
  QApplication app(argc, argv);
  const QString executable = root.filePath("hockey stick \"$`%\\-ui");
  const QString launch_result = root.filePath("launched");
  qputenv("HSTREAM_ICON_LAUNCH_RESULT", launch_result.toUtf8());
  if (!write_file(executable, "#!/bin/sh\nprintf launched > \"$HSTREAM_ICON_LAUNCH_RESULT\"\n") ||
      !QFile::setPermissions(executable, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
    return 1;
  const QString error = hm::ui_internal::ensure_desktop_integration(executable);
  const QString launcher = QDir(data_home).filePath("applications/hstream-ui.desktop");
  const QString icon_path = QDir(data_home).filePath("hstream-ui/hstream-ui.png");
  const QByteArray generated = read_file(launcher);
  QImage icon(icon_path);
  bool ok = expect(error.isEmpty(), error.toStdString().c_str()) &&
      expect(generated.contains("\nStartupWMClass=" + QGuiApplication::desktopFileName().toUtf8() + "\n") &&
                 generated.contains("\nIcon=" + icon_path.toUtf8() + "\n") && !icon.isNull() &&
                 icon.size() == QSize(256, 256),
             "an uninstalled build must register a desktop entry matching its identity and a usable absolute icon");
  if (!ok)
    std::cerr << generated.toStdString();

  // Exercise freedesktop Exec parsing, including spaces, quotes, percent field
  // codes and shell metacharacters, using the desktop's actual launcher.
  const QString gio = QStandardPaths::findExecutable("gio");
  if (!gio.isEmpty()) {
    QProcess launch;
    launch.start(gio, {"launch", launcher});
    ok &= expect(launch.waitForFinished(5000) && launch.exitCode() == 0, "the generated launcher must execute via GIO");
    if (launch.exitCode() != 0)
      std::cerr << launch.readAllStandardError().toStdString();
    QElapsedTimer timeout;
    timeout.start();
    while (!QFileInfo::exists(launch_result) && timeout.elapsed() < 5000)
      QThread::msleep(10);
    ok &=
        expect(read_file(launch_result) == "launched", "desktop Exec quoting must preserve the exact executable path");
  }

  const QDateTime old_time = QDateTime::fromSecsSinceEpoch(1'000'000'000);
  QFile unchanged(launcher);
  ok &= unchanged.open(QIODevice::ReadOnly) && unchanged.setFileTime(old_time, QFileDevice::FileModificationTime);
  unchanged.close();
  ok &= expect(
      hm::ui_internal::ensure_desktop_integration(executable).isEmpty() &&
          QFileInfo(launcher).lastModified() == old_time && read_file(launcher) == generated,
      "unchanged startup registration must not rewrite the desktop entry");
  const QString next_executable = root.filePath("new-build/hstream-ui");
  ok &= expect(
      hm::ui_internal::ensure_desktop_integration(next_executable).isEmpty() && read_file(launcher) != generated &&
          read_file(launcher).contains("new-build/hstream-ui"),
      "a managed launcher must follow a later build's executable path");

  const QByteArray custom = "[Desktop Entry]\nType=Application\nName=Custom HStream\nHidden=true\n";
  ok &= write_file(launcher, custom);
  ok &= expect(
      hm::ui_internal::ensure_desktop_integration(executable).isEmpty() && read_file(launcher) == custom,
      "custom launchers and explicit hiding must remain unchanged");
  QFile::remove(launcher);
  ok &= hm::ui_internal::ensure_desktop_integration(executable).isEmpty();
  QDir().mkpath(QDir(system_data).filePath("applications"));
  const QString installed_launcher = QDir(system_data).filePath("applications/hstream-ui.desktop");
  ok &= write_file(installed_launcher, "[Desktop Entry]\nName=HStream\nIcon=hstream-ui\n");
  ok &= expect(
      hm::ui_internal::ensure_desktop_integration(executable).isEmpty() && !QFileInfo::exists(launcher),
      "a package installation must supersede the managed development launcher");
  ok &= expect(
      hm::ui_internal::ensure_desktop_integration(executable).isEmpty() && !QFileInfo::exists(launcher),
      "command-line launch must use an existing system desktop entry");

  const QString artifacts = qEnvironmentVariable("HSTREAM_ICON_ARTIFACT_DIR");
  if (!artifacts.isEmpty()) {
    QDir().mkpath(artifacts);
    for (const int size : {16, 24, 32, 48, 256, 512})
      ok &= hm::ui_internal::application_icon()
                .pixmap(size, size)
                .save(QDir(artifacts).filePath(QString("hstream-icon-%1.png").arg(size)));
  }
  return ok ? 0 : 1;
}
