#include "src/apps/hstream-ui/HStreamWindow.h"

#include <QtCore/QtGlobal>
#include <QtWidgets/QApplication>

int main(int argc, char** argv) {
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
  HStreamWindow window;
  app.setWindowIcon(window.windowIcon());
  window.show();
  return app.exec();
}
