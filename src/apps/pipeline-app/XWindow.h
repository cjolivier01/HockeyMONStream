#pragma once

/* clang-format off */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#undef Status
/* clang-format on */

#include <string>

namespace hm {

class XWindow {
 public:
  // Constructor creates (or reuses) the X Display and then creates a window.
  // 'x' and 'y' specify the window position; 'width' and 'height' are its dimensions;
  // 'title' is the window title and 'event_mask' determines which events are handled.
  XWindow(int x, int y, unsigned int width, unsigned int height, const std::string& title, long event_mask);

  // Destructor destroys the window.
  ~XWindow();

  // Returns the underlying X11 Window.
  Window getWindow() const;

  // Returns the shared Display pointer.
  static Display* getDisplay();

  // Closes the display connection (should be called once when all windows are done).
  static void CloseDisplay();

 private:
  // Helper that creates the window and sets its properties.
  void createWindow(int x, int y, unsigned int width, unsigned int height, const std::string& title, long event_mask);

  // The X11 window created.
  Window window_;

  // A shared display pointer among all XWindow instances.
  static Display* s_display;
};

} // namespace hm
