#include "XWindow.h"

#include <cstring>
#include <stdexcept>

namespace hm {

Display* XWindow::s_display = nullptr;

XWindow::XWindow(int x, int y, unsigned int width, unsigned int height, const std::string& title, long event_mask) {
  // Open the display if not already open.
  if (!s_display) {
    s_display = XOpenDisplay(nullptr);
    if (!s_display) {
      throw std::runtime_error("Failed to open X Display");
    }
  }
  createWindow(x, y, width, height, title, event_mask);
}

void XWindow::createWindow(
    int x,
    int y,
    unsigned int width,
    unsigned int height,
    const std::string& title,
    long event_mask) {
  int screen = DefaultScreen(s_display);
  window_ = XCreateSimpleWindow(s_display, RootWindow(s_display, screen), x, y, width, height, 2, 0, 0);

  // Set window hints (position and size).
  XSizeHints hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.flags = PPosition | PSize;
  hints.x = x;
  hints.y = y;
  hints.width = width;
  hints.height = height;
  XSetNormalHints(s_display, window_, &hints);

  // Set window title.
  XTextProperty xproperty;
  char* c_title = const_cast<char*>(title.c_str());
  if (XStringListToTextProperty(&c_title, 1, &xproperty)) {
    XSetWMName(s_display, window_, &xproperty);
    XFree(xproperty.value);
  }

  // Set event mask.
  XSetWindowAttributes attr;
  attr.event_mask = event_mask;
  XChangeWindowAttributes(s_display, window_, CWEventMask, &attr);

  // Set the WM_DELETE_WINDOW protocol so the window manager can close the window.
  Atom wmDeleteMessage = XInternAtom(s_display, "WM_DELETE_WINDOW", False);
  if (wmDeleteMessage != None) {
    XSetWMProtocols(s_display, window_, &wmDeleteMessage, 1);
  }

  // Map (display) the window.
  XMapRaised(s_display, window_);
  XSync(s_display, False);
}

XWindow::~XWindow() {
  if (s_display && window_) {
    XDestroyWindow(s_display, window_);
    window_ = 0;
  }
}

Window XWindow::getWindow() const {
  return window_;
}

Display* XWindow::getDisplay() {
  return s_display;
}

void XWindow::CloseDisplay() {
  if (s_display) {
    XCloseDisplay(s_display);
    s_display = nullptr;
  }
}
} // namespace hm
