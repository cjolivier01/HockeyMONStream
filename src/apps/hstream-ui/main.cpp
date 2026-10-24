#include "src/apps/hstream-ui/ActionIcons.h"
#include "src/apps/hstream-ui/BackendOptions.h"
#include "src/apps/hstream-ui/HStreamWindow.h"

#include <QtCore/QCommandLineParser>
#include <QtCore/QDebug>
#include <QtCore/QtGlobal>
#include <QtGui/QIcon>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>

#include <exception>
#include <iostream>

#include "hstream/src/libs/common/ProcessDiagnostics.h"
#include "src/apps/hstream-ui/UiDiagnostics.h"

int main(int argc, char** argv) {
  // Help must also work on a headless machine with no display server.
  for (int index = 1; index < argc; ++index) {
    if (QString::fromLocal8Bit(argv[index]) == "--help" || QString::fromLocal8Bit(argv[index]) == "-h") {
      std::cout << "Usage: hstream-ui [options]\n"
                   "  --backend hstream|hockeymon    Program processing backend (default: hstream)\n"
                   "  --backend-runner PATH         Absolute backend executable\n"
                   "  --backend-launch PATH         Initial HockeyMON launch document\n"
                   "  --game-id ID                  Select an existing game\n"
                   "  --play                        Start the selected game\n"
                   "  --stitching-calibration-only   Open shared native calibration mode\n";
      return 0;
    }
  }
  hm::diagnostics::Initialize("hstream-ui", argc ? argv[0] : nullptr);
  install_qt_diagnostics();
  hm::ui_internal::configure_application_identity();
#if defined(Q_OS_LINUX)
  // DeepStream's EGL render sink consumes an X11 Window handle. On a Wayland
  // desktop Qt otherwise returns a wl_surface pointer from winId(), which the
  // pipeline cannot use as an XID. XWayland provides the compatible embedding
  // path until the renderer supports native Wayland handles.
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && !qEnvironmentVariableIsEmpty("DISPLAY")) {
    qputenv("QT_QPA_PLATFORM", "xcb");
  }
#endif
  QApplication app(argc, argv);
  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addOptions({
      {QStringList{"backend"}, "Program processing backend", "name", "hstream"},
      {QStringList{"backend-runner"}, "Absolute Program backend executable", "path"},
      {QStringList{"backend-launch"}, "Private initial HockeyMON launch document", "path"},
      {QStringList{"game-id", "g"}, "Select an existing game", "id"},
      {QStringList{"play"}, "Start the selected game"},
      {QStringList{"stitching-calibration-only"}, "Open shared native calibration mode"},
  });
  parser.process(app);
  UiDiagnostics diagnostics;
  hm::diagnostics::Breadcrumb("ui", "QApplication ready");
  if (!hm::diagnostics::Directory().empty())
    qInfo().noquote() << "HStream diagnostics:" << QString::fromStdString(hm::diagnostics::Directory());
  install_button_icon_style();
  app.setWindowIcon(hm::ui_internal::application_icon());
  const QString desktop_error = hm::ui_internal::ensure_desktop_integration(QCoreApplication::applicationFilePath());
  if (!desktop_error.isEmpty())
    qWarning().noquote() << "HStream desktop integration:" << desktop_error;
  try {
    const hm::ui::BackendOptions backend{
        parser.value("backend"), parser.value("backend-runner"), parser.value("backend-launch")};
    backend.verifyRunner();
    HStreamWindow window(nullptr, backend);
    window.openGame(parser.value("game-id"), parser.isSet("play"), parser.isSet("stitching-calibration-only"));
    window.show();
    const int result = app.exec();
    hm::diagnostics::Finish(result);
    return result;
  } catch (const std::exception& error) {
    hm::diagnostics::Log("startup-error", error.what());
    hm::diagnostics::Finish(1);
    QMessageBox::critical(nullptr, "HStream configuration error", error.what());
    return 1;
  }
}
