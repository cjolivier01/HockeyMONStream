#pragma once

#include <filesystem>

#include <QtGui/QPixmap>
#include <QtWidgets/QGraphicsView>

class QGraphicsPixmapItem;
class QGraphicsTextItem;

// Independent navigation for bounded, saved calibration images. No video readback.
class CalibrationFrameView : public QGraphicsView {
 public:
  explicit CalibrationFrameView(QWidget* parent = nullptr);
  void load(const std::filesystem::path& path, int maximum_width = 1024);
  void fitImage();
  void actualSize();
  void zoomIn();
  void zoomOut();
  QPixmap pixmap() const;
  QString message() const;

 protected:
  void resizeEvent(QResizeEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;

 private:
  void zoom(double factor);
  void allowPan();
  void fit();
  QGraphicsPixmapItem* image_;
  QGraphicsTextItem* message_;
  QString loaded_path_;
  bool fit_to_window_{true};
};
