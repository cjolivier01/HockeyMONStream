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
  // GL sinks can create a native child under the supplied window. ClearArea
  // clips those children, leaving a stopped renderer's old frame visible.
  // IncludeInferiors clears that front buffer too, without waking the renderer
  // or reading its pixels. The server clips this rectangle to the actual size.
  const auto gc = xcb_generate_id(connection);
  const uint32_t values[] = {black, XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS};
  xcb_create_gc(connection, gc, window, XCB_GC_FOREGROUND | XCB_GC_SUBWINDOW_MODE, values);
  const xcb_rectangle_t rectangle{0, 0, UINT16_MAX, UINT16_MAX};
  xcb_poly_fill_rectangle(connection, window, gc, 1, &rectangle);
  xcb_free_gc(connection, gc);
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
