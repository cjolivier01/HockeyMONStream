#pragma once

#include <QtGui/QImage>
#include <QtWidgets/QWidget>

namespace hm::ui {

// Native video sinks own their pixels while WA_PaintOnScreen is set. Qt must
// not repaint incoming frames, but a geometry change needs a defined background
// even when the renderer is idle or its process is SIGSTOP-paused.
class NativePreviewSurface : public QWidget {
 public:
  using QWidget::QWidget;

  void setImageMode(bool enabled);
  bool imageMode() const {
    return image_mode_;
  }
  void setImage(const QImage& image);
  QPaintEngine* paintEngine() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;

 private:
  void clearNativeSurface();
  bool image_mode_{false};
  bool native_paint_on_screen_{false};
  bool native_no_system_background_{false};
  QImage image_;
};

} // namespace hm::ui
