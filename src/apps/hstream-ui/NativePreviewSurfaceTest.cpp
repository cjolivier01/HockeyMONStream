#include "src/apps/hstream-ui/NativePreviewSurface.h"

#include <QtGui/qguiapplication_platform.h>
#include <xcb/xcb.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QPaintEngine>
#include <QtWidgets/QApplication>

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
class Surface : public hm::ui::NativePreviewSurface {
 public:
  Surface() {
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
  }
  QPaintEngine* paintEngine() const override {
    return nullptr;
  }
};

class PostedEvent : public QObject {
 public:
  bool delivered{false};
  bool event(QEvent* event) override {
    if (event->type() == QEvent::User)
      delivered = true;
    return QObject::event(event);
  }
};
} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  if (QGuiApplication::platformName() != "xcb") {
    std::cerr << "Run with QT_QPA_PLATFORM=xcb and an X server\n";
    return 1;
  }
  Surface surface;
  surface.resize(320, 180);
  surface.show();
  QGuiApplication::sync();
  auto* connection = qGuiApp->nativeInterface<QNativeInterface::QX11Application>()->connection();
  const auto window = static_cast<xcb_window_t>(surface.winId());
  const auto gc = xcb_generate_id(connection);
  const uint32_t magenta = 0xff00ff;
  xcb_create_gc(connection, gc, window, XCB_GC_FOREGROUND, &magenta);
  auto fill = [&] {
    const xcb_rectangle_t rect{0, 0, static_cast<uint16_t>(surface.width()), static_cast<uint16_t>(surface.height())};
    xcb_poly_fill_rectangle(connection, window, gc, 1, &rect);
    xcb_flush(connection);
  };
  auto pixel = [&](int x, int y) {
    auto* reply = xcb_get_image_reply(
        connection, xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window, x, y, 1, 1, ~0U), nullptr);
    uint32_t value = ~0U;
    if (reply && xcb_get_image_data_length(reply) >= 4)
      std::memcpy(&value, xcb_get_image_data(reply), 4);
    std::free(reply);
    return value & 0xffffff;
  };
  auto expect = [](bool value, const char* message) {
    if (!value)
      std::cerr << message << '\n';
    return value;
  };
  bool ok = true;
  for (const QSize size : {QSize(480, 270), QSize(240, 135)}) {
    fill();
    ok &= expect(pixel(20, 20) == magenta, "Fixture must contain stale native pixels");
    surface.update();
    QGuiApplication::sync();
    ok &= expect(pixel(20, 20) == magenta, "Ordinary Qt updates must preserve externally rendered frames");
    PostedEvent probe;
    QCoreApplication::postEvent(&probe, new QEvent(QEvent::User));
    surface.resize(size);
    ok &= expect(!probe.delivered, "Resize must not drain posted events");
    QGuiApplication::sync();
    ok &= expect(probe.delivered, "The normal event loop must deliver the probe");
    ok &= expect(
        pixel(20, 20) == 0 && pixel(size.width() - 2, size.height() - 2) == 0,
        "Growing and shrinking idle previews must clear all stale native pixels");
  }
  fill();
  surface.hide();
  surface.show();
  QGuiApplication::sync();
  ok &= expect(pixel(20, 20) == 0, "Remapping must clear stale pixels without waiting for a video frame");
  xcb_free_gc(connection, gc);
  return ok ? 0 : 1;
}
