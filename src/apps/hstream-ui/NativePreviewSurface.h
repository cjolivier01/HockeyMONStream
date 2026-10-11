#pragma once

#include <QtWidgets/QWidget>

namespace hm::ui {

// Native video sinks own their pixels while WA_PaintOnScreen is set. Qt must
// not repaint incoming frames, but a geometry change needs a defined background
// even when the renderer is idle or its process is SIGSTOP-paused.
class NativePreviewSurface : public QWidget {
 public:
  using QWidget::QWidget;

 protected:
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;

 private:
  void clearNativeSurface();
};

} // namespace hm::ui
