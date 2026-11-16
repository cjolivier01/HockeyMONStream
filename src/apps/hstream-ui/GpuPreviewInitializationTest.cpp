#include "hstream/src/apps/apps-common/HmGpuPreview.h"

#include <QtWidgets/QApplication>

#include <X11/Xlib.h>
#include <gst/gst.h>

#include <iostream>
#include <string>

namespace {
std::string x11_diagnostics;

int ignore_x_error(Display*, XErrorEvent*) {
  return 0;
}

void capture_diagnostic(const gchar* message) {
  x11_diagnostics += message;
}
} // namespace

int main(int argc, char** argv) {
  hm::gpu_preview::initialize_process();
  QApplication application(argc, argv);
  if (application.platformName() != "xcb") {
    std::cerr << "This regression requires real Qt XCB initialization\n";
    return 1;
  }
  const auto qt_handler = XSetErrorHandler(ignore_x_error);
  XSetErrorHandler(qt_handler);
  gst_init(&argc, &argv);
  if (!hm::gpu_preview::register_elements() || !hm::gpu_preview::register_elements())
    return 1;
  const auto preview_handler = XSetErrorHandler(ignore_x_error);
  XSetErrorHandler(preview_handler);
  if (preview_handler == qt_handler) {
    std::cerr << "Qt's X11 error handler still owns preview errors\n";
    return 1;
  }

  Display* display = XOpenDisplay(nullptr);
  if (!display)
    return 1;
  const Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 16, 16, 0, 0, 0);
  XDestroyWindow(display, window);
  XSync(display, False);
  const auto previous_diagnostic = g_set_printerr_handler(capture_diagnostic);
  XWindowAttributes attributes{};
  const int valid = XGetWindowAttributes(display, window, &attributes);
  XSync(display, False);
  g_set_printerr_handler(previous_diagnostic);
  XCloseDisplay(display);
  if (valid || x11_diagnostics.find("HSTREAM_PREVIEW_X11_ERROR code=3 ") == std::string::npos) {
    std::cerr << "Destroyed drawable did not reach the preview BadWindow handler: " << x11_diagnostics;
    return 1;
  }
  return 0;
}
