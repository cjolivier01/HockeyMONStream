#include "src/apps/hstream-ui/NativePreviewSurface.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#if QT_CONFIG(xcb)
#include <QtGui/qguiapplication_platform.h>
#include <xcb/xcb.h>
#endif

namespace hm::ui {

void NativePreviewSurface::setImageMode(bool enabled) {
  if (enabled == image_mode_)
    return;
  if (enabled) {
    native_paint_on_screen_ = testAttribute(Qt::WA_PaintOnScreen);
    native_no_system_background_ = testAttribute(Qt::WA_NoSystemBackground);
  }
  image_mode_ = enabled;
  image_ = {};
  setAttribute(Qt::WA_PaintOnScreen, enabled ? false : native_paint_on_screen_);
  setAttribute(Qt::WA_NoSystemBackground, enabled ? false : native_no_system_background_);
  update();
}

void NativePreviewSurface::setImage(const QImage& image) {
  if (!image_mode_)
    return;
  image_ = image;
  update();
}

QPaintEngine* NativePreviewSurface::paintEngine() const {
  return testAttribute(Qt::WA_PaintOnScreen) ? nullptr : QWidget::paintEngine();
}

void NativePreviewSurface::paintEvent(QPaintEvent* event) {
  if (!image_mode_) {
    QWidget::paintEvent(event);
    return;
  }
  QPainter painter(this);
  painter.fillRect(rect(), Qt::black);
  if (!image_.isNull()) {
    const QSize size = image_.size().scaled(this->size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(target, image_);
  }
}

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
