#include "src/apps/hstream-ui/CalibrationFrameView.h"

#include <algorithm>
#include <cmath>

#include <QtCore/QFileInfo>
#include <QtGui/QImageReader>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QGraphicsPixmapItem>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QGraphicsTextItem>

CalibrationFrameView::CalibrationFrameView(QWidget* parent) : QGraphicsView(parent) {
  setScene(new QGraphicsScene(this));
  image_ = scene()->addPixmap({});
  message_ = scene()->addText({});
  message_->setDefaultTextColor(Qt::white);
  setBackgroundBrush(QColor("#111111"));
  setMinimumSize(180, 160);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  setDragMode(QGraphicsView::ScrollHandDrag);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setRenderHint(QPainter::SmoothPixmapTransform);
  setResizeAnchor(QGraphicsView::AnchorViewCenter);
  setToolTip("Wheel to zoom at the pointer; drag to pan. Double-click or Fit to show the whole image.");
}

void CalibrationFrameView::load(const std::filesystem::path& path, int maximum_width) {
  const QString file = QString::fromStdString(path.string());
  if (loaded_path_ == file && !image_->pixmap().isNull())
    return;
  loaded_path_.clear();
  image_->setPixmap({});
  message_->setPlainText({});
  resetTransform();
  const QFileInfo info(file);
  if (!info.isFile()) {
    message_->setPlainText(
        maximum_width == 1024 ? "Thumbnail becomes available after calibration."
                              : "Match visualization is unavailable for this pair.");
  } else if (info.isSymLink() || info.size() > 5 * 1024 * 1024) {
    message_->setPlainText("Image exceeds the inspection limits.");
  } else {
    QImageReader reader(file);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > maximum_width || size.height() > 1024) {
      message_->setPlainText("Image exceeds the inspection limits.");
    } else {
      image_->setPixmap(QPixmap::fromImage(reader.read()));
      if (image_->pixmap().isNull())
        message_->setPlainText("Image could not be read.");
      else
        loaded_path_ = file;
    }
  }
  message_->setVisible(image_->pixmap().isNull());
  fitImage();
}

QPixmap CalibrationFrameView::pixmap() const {
  return image_->pixmap();
}
QString CalibrationFrameView::message() const {
  return message_->toPlainText();
}

void CalibrationFrameView::fit() {
  if (image_->pixmap().isNull()) {
    message_->setTextWidth(std::max(1, viewport()->width() - 8));
    scene()->setSceneRect(message_->boundingRect());
    setSceneRect(scene()->sceneRect());
    return;
  }
  scene()->setSceneRect(image_->boundingRect());
  setSceneRect(scene()->sceneRect());
  resetTransform();
  const double factor = std::min(
      std::max(1, viewport()->width() - 4) / image_->boundingRect().width(),
      std::max(1, viewport()->height() - 4) / image_->boundingRect().height());
  scale(factor, factor);
  centerOn(image_);
}

void CalibrationFrameView::fitImage() {
  fit_to_window_ = true;
  fit();
}
void CalibrationFrameView::actualSize() {
  if (image_->pixmap().isNull())
    return;
  fit_to_window_ = false;
  resetTransform();
  // One saved image pixel per physical display pixel, including high-DPI screens.
  scale(1.0 / devicePixelRatioF(), 1.0 / devicePixelRatioF());
  allowPan();
  centerOn(image_);
}
void CalibrationFrameView::allowPan() {
  // Keep scrollable margins even when an image dimension fits the viewport;
  // otherwise Qt's centering cancels pointer-anchored zoom and prevents panning.
  const double margin_x = viewport()->width() / transform().m11();
  const double margin_y = viewport()->height() / transform().m22();
  setSceneRect(image_->boundingRect().adjusted(-margin_x, -margin_y, margin_x, margin_y));
}
void CalibrationFrameView::zoom(double factor) {
  if (image_->pixmap().isNull())
    return;
  fit_to_window_ = false;
  const QPoint center = viewport()->rect().center();
  const QPointF before = mapToScene(center);
  setTransformationAnchor(NoAnchor);
  const double current = transform().m11();
  const double target = std::clamp(current * factor, 0.02, 32.0);
  scale(target / current, target / current);
  allowPan();
  const QPointF after = mapToScene(center);
  translate(after.x() - before.x(), after.y() - before.y());
}
void CalibrationFrameView::zoomIn() {
  zoom(1.25);
}
void CalibrationFrameView::zoomOut() {
  zoom(0.8);
}
void CalibrationFrameView::resizeEvent(QResizeEvent* event) {
  QGraphicsView::resizeEvent(event);
  if (fit_to_window_)
    fit();
  else
    allowPan();
}
void CalibrationFrameView::wheelEvent(QWheelEvent* event) {
  if (image_->pixmap().isNull()) {
    event->ignore();
    return;
  }
  const int delta = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->pixelDelta().y();
  if (delta == 0) {
    event->ignore();
    return;
  }
  const QPoint position = event->position().toPoint();
  const QPointF before = mapToScene(position);
  zoom(std::pow(1.25, std::clamp(delta / 120.0, -4.0, 4.0)));
  const QPointF after = mapToScene(position);
  translate(after.x() - before.x(), after.y() - before.y());
  event->accept();
}
void CalibrationFrameView::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    fitImage();
    event->accept();
  } else {
    QGraphicsView::mouseDoubleClickEvent(event);
  }
}
