#pragma once

#include <QtWidgets/QDialog>

class RinkMaskCanvas;
class QImage;

class RinkMaskPaintDialog : public QDialog {
 public:
  RinkMaskPaintDialog(const QImage& preview, QWidget* parent);
  QByteArray response() const;

 private:
  RinkMaskCanvas* canvas_;
};
