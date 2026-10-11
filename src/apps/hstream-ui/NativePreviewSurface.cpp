#include "src/apps/hstream-ui/NativePreviewSurface.h"

#include <QtGui/QGuiApplication>
#if QT_CONFIG(xcb)
#include <QtGui/qguiapplication_platform.h>
#include <xcb/xcb.h>
#endif

namespace hm::ui {

void NativePreviewSurface::clearNativeSurface() {
#if QT_CONFIG(xcb)
  if (!testAttribute(Qt::WA_PaintOnScreen) || QGuiApplication::platformName() != "xcb")
    return;
  auto* x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
  if (!x11 || !internalWinId())
    return;
  auto* connection = x11->connection();
  const auto window = static_cast<xcb_window_t>(internalWinId());
  const uint32_t black = 0;
  xcb_change_window_attributes(connection, window, XCB_CW_BACK_PIXEL, &black);
  // Zero width/height clears to the current window edges, including newly
  // exposed pixels. The server also uses this background for later exposes.
  // Stay on Qt's connection so geometry and clears remain ordered. Never pump
  // posted events from a geometry callback (including QGuiApplication::sync).
  xcb_clear_area(connection, 0, window, 0, 0, 0, 0);
  xcb_flush(connection);
#endif
}

void NativePreviewSurface::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  clearNativeSurface();
}

void NativePreviewSurface::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  clearNativeSurface();
}

} // namespace hm::ui
