#include "src/apps/hstream-ui/RinkMaskPaintDialog.h"
#include <QtTest/qtest_widgets.h>
#include <QtTest/qtestmouse.h>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
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
  painter.end();
  const QImage source = still.copy();
  painter.begin(&still);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(60, 220, 80, 95));
  painter.drawRoundedRect(QRectF(89, 60, 622, 292), 40, 40);
  painter.end();

  RinkMaskPaintDialog dialog(still, source, nullptr);
  dialog.show();
  QApplication::processEvents();
  auto* canvas = dialog.findChild<QWidget*>("rinkMaskCanvas");
  auto* mode = dialog.findChild<QComboBox*>();
  auto* size = dialog.findChild<QSpinBox*>();
  QPushButton* undo = nullptr;
  for (auto* button : dialog.findChildren<QPushButton*>())
    if (button->text() == "Undo stroke")
      undo = button;
  if (!canvas || !size || !mode || mode->currentText() != "Paint" || !undo || dialog.response() != "keep\n")
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
  auto color = [&](int x, int y) { return canvas->grab().toImage().pixelColor(point(x, y)); };
  QImage unmasked(canvas->size(), QImage::Format_RGB32);
  QPainter render(&unmasked);
  render.drawImage(
      QRectF((canvas->width() - fit.width()) / 2., (canvas->height() - fit.height()) / 2., fit.width(), fit.height()),
      source);
  render.end();
  mode->setCurrentText("Erase");
  size->setValue(27);
  QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, point(400, 150));
  QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, point(400, 270));
  if (!dialog.response().endsWith("-27 400 150 400 270\n"))
    return 1;
  for (int x = 390; x <= 410; ++x)
    if (color(x, 220) != unmasked.pixelColor(point(x, 220))) {
      std::cerr << "Erase did not reveal the source at x=" << x << '\n';
      return 1;
    }
  if (!prefix.isEmpty()) {
    dialog.grab().save(prefix + "-after.png");
    still.save(prefix + "-detected.png");
    source.save(prefix + "-source.png");
  }
  QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, point(73, 247));
  if (color(73, 247) != source.pixelColor(73, 247))
    return 1;
  undo->click();
  if (color(73, 247) != QColor(65, 210, 235))
    return 1;
  undo->click();
  if (color(407, 220) != still.pixelColor(407, 220))
    return 1;
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
