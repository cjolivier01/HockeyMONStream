#include "src/apps/hstream-ui/ValueControlWheel.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QAbstractScrollArea>
#include <QtWidgets/QAbstractSpinBox>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTabBar>

namespace {

QWidget* value_control(QWidget* widget) {
  for (QWidget* current = widget; current; current = current->parentWidget()) {
    // Open popup lists and text/log viewports scroll their contents normally.
    if (qobject_cast<QAbstractScrollArea*>(current) || qobject_cast<QScrollBar*>(current))
      return nullptr;
    if (qobject_cast<QAbstractSpinBox*>(current) || qobject_cast<QComboBox*>(current) ||
        qobject_cast<QAbstractSlider*>(current) || qobject_cast<QLineEdit*>(current) ||
        qobject_cast<QAbstractButton*>(current) || qobject_cast<QTabBar*>(current))
      return current;
    if (current->isWindow())
      break;
  }
  return nullptr;
}

bool scroll_pane(QAbstractScrollArea* pane, QWheelEvent* wheel) {
  QPoint pixels = wheel->pixelDelta();
  QPoint angles = wheel->angleDelta();
  if (wheel->modifiers().testFlag(Qt::ShiftModifier) && !pixels.x() && !angles.x()) {
    pixels = QPoint(pixels.y(), 0);
    angles = QPoint(angles.y(), 0);
  }
  bool consumed = false;
  for (Qt::Orientation axis : {Qt::Horizontal, Qt::Vertical}) {
    QScrollBar* bar = axis == Qt::Horizontal ? pane->horizontalScrollBar() : pane->verticalScrollBar();
    const int pixel_delta = axis == Qt::Horizontal ? pixels.x() : pixels.y();
    const int angle_delta = axis == Qt::Horizontal ? angles.x() : angles.y();
    const int delta = pixel_delta ? pixel_delta : angle_delta;
    if (!bar->isEnabled() || !delta || (delta > 0 ? bar->value() <= bar->minimum() : bar->value() >= bar->maximum()))
      continue;
    if (pixel_delta) {
      bar->setValue(bar->value() - pixel_delta);
      consumed = true;
    } else {
      // Scroll bars retain Qt's platform step/angle accumulation. Sending to
      // the viewport could instead trigger an enclosing canvas's zoom handler.
      QWheelEvent forwarded(
          bar->mapFromGlobal(wheel->globalPosition().toPoint()),
          wheel->globalPosition(),
          {},
          axis == Qt::Horizontal ? QPoint(angle_delta, 0) : QPoint(0, angle_delta),
          wheel->buttons(),
          wheel->modifiers() & ~Qt::ShiftModifier,
          wheel->phase(),
          wheel->inverted(),
          wheel->source());
      forwarded.ignore();
      QCoreApplication::sendEvent(bar, &forwarded);
      consumed = forwarded.isAccepted() || consumed;
    }
  }
  return consumed;
}

class ValueControlWheelGuard final : public QObject {
 public:
  using QObject::QObject;

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    return route_value_control_wheel(nullptr, watched, event);
  }
};

} // namespace

bool route_value_control_wheel(QWidget* root, QObject* watched, QEvent* event) {
  if (!event || event->type() != QEvent::Wheel)
    return false;
  auto* widget = qobject_cast<QWidget*>(watched);
  if (!widget || (root && widget != root && !root->isAncestorOf(widget)))
    return false;
  QWidget* control = value_control(widget);
  if (!control)
    return false;
  auto* wheel = static_cast<QWheelEvent*>(event);
  for (QWidget* parent = control->parentWidget(); parent; parent = parent->parentWidget()) {
    if (auto* pane = qobject_cast<QAbstractScrollArea*>(parent); pane && scroll_pane(pane, wheel))
      break;
    if (parent->isWindow())
      break;
  }
  // No scrolling ancestor is still not permission to edit a value.
  wheel->accept();
  return true;
}

void install_value_control_wheel_guard() {
  static QPointer<QObject> guard;
  if (!qApp || guard)
    return;
  guard = new ValueControlWheelGuard(qApp);
  qApp->installEventFilter(guard);
}
