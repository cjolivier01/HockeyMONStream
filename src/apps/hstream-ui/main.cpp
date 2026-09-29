#include "src/apps/hstream-ui/ActionIcons.h"
#include "src/apps/hstream-ui/HStreamWindow.h"

#include <QtCore/QDebug>
#include <QtCore/QtGlobal>
#include <QtGui/QIcon>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>

#include <exception>
#include <iostream>
#include <string_view>

#include "hstream/src/libs/common/ProcessDiagnostics.h"
#include "hstream/src/libs/stitching/ScoreboardSelector.h"
#include "src/apps/hstream-ui/UiDiagnostics.h"

int main(int argc, char** argv) {
  hm::diagnostics::Initialize("hstream-ui", argc ? argv[0] : nullptr);
  // The Scoreboard tab starts this short-lived child so selection retains the
  // native selector's generation checks without blocking Qt's event loop.
  if (argc == 3 && std::string_view(argv[1]) == "--scoreboard-select") {
    const absl::Status status = hm::stitching::ScoreboardSelector::Run(argv[2]);
    if (!status.ok()) {
      std::cerr << "Scoreboard selection: " << status << '\n';
      return status.code() == absl::StatusCode::kCancelled ? 0 : 1;
    }
    std::cerr << "Scoreboard selection saved\n";
    return 0;
  }
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
    HStreamWindow window;
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
