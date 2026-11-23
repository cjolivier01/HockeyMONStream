#include "src/apps/hstream-ui/RinkMaskPaintDialog.h"
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtTest/qtest_widgets.h>
#include <QtTest/qtestmouse.h>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <iostream>

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QImage still(800, 400, QImage::Format_RGB32);
  still.fill(QColor("#374857"));
  QPainter painter(&still);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setBrush(QColor("#e5edf0"));
  painter.setPen(QPen(Qt::white, 5));
  painter.drawRoundedRect(QRectF(24, 56, 752, 300), 75, 75);
  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(QColor("#7c9ebb"), 3));
  painter.drawEllipse(QPointF(400, 206), 45, 45);
  for (int x : {260, 540})
    painter.drawLine(x, 56, x, 356);
  painter.setPen(QPen(QColor("#cf8a88"), 3));
  painter.drawLine(400, 56, 400, 356);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(60, 220, 80, 95));
  painter.drawRoundedRect(QRectF(89, 60, 622, 292), 40, 40);
  painter.end();

  RinkMaskPaintDialog dialog(still, nullptr);
  dialog.show();
  QApplication::processEvents();
  auto* canvas = dialog.findChild<QWidget*>("rinkMaskCanvas");
  auto* size = dialog.findChild<QSpinBox*>();
  QPushButton* undo = nullptr;
  for (auto* button : dialog.findChildren<QPushButton*>())
    if (button->text() == "Undo stroke")
      undo = button;
  if (!canvas || !size || !undo || dialog.response() != "keep\n")
    return 1;
  const QSize fit = still.size().scaled(canvas->size(), Qt::KeepAspectRatio);
  auto point = [&](int x, int y) {
    return QPoint(
        qRound((canvas->width() - fit.width()) / 2. + (x + .5) * fit.width() / still.width()),
        qRound((canvas->height() - fit.height()) / 2. + (y + .5) * fit.height() / still.height()));
  };
  const QString prefix = qEnvironmentVariable("HSTREAM_PAINT_SCREENSHOT_PREFIX");
  if (!prefix.isEmpty())
    dialog.grab().save(prefix + "-before.png");
  size->setValue(17);
  QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, point(73, 247));
  size->setValue(33);
  QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, point(742, 180));
  QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, point(735, 260));
  const auto strokes = dialog.response().trimmed().split('\n');
  if (strokes.size() != 2 || strokes.front() != "17 73 247" || strokes.back() != "33 742 180 735 260") {
    std::cerr << dialog.response().data();
    return 1;
  }
  if (!prefix.isEmpty())
    dialog.grab().save(prefix + "-after.png");
  undo->click();
  if (dialog.response().trimmed() != strokes.front())
    return 1;
  undo->click();
  if (dialog.response() != "keep\n" || undo->isEnabled())
    return 1;
  for (auto* button : dialog.findChildren<QPushButton*>())
    if (button->text() == "Use detected mask")
      button->click();
  if (dialog.result() != RinkMaskPaintDialog::UseDetectedMask)
    return 1;
  dialog.show();
  QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, point(73, 247));
  QTest::keyClick(&dialog, Qt::Key_Escape);
  return !dialog.isVisible() && dialog.result() == QDialog::Rejected ? 0 : 1;
}
