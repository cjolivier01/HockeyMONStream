#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QAbstractScrollArea>
#include <QtWidgets/QAbstractSpinBox>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTabBar>

// Share the main window's wheel behavior with independent dialog windows.
// Focus never enables value changes; only the nearest scrolling pane moves.
// Image canvases and ordinary scroll bars keep their existing wheel behavior.
inline bool route_value_control_wheel(QWidget* root, QObject* watched, QEvent* event) {
  if (root && event && event->type() == QEvent::Wheel) {
    auto* widget = qobject_cast<QWidget*>(watched);
    const bool belongs_to_window = widget && (widget == root || root->isAncestorOf(widget));
    const bool changes_value = qobject_cast<QAbstractSpinBox*>(widget) || qobject_cast<QComboBox*>(widget) ||
        (qobject_cast<QAbstractSlider*>(widget) && !qobject_cast<QScrollBar*>(widget)) ||
        qobject_cast<QLineEdit*>(widget) || qobject_cast<QRadioButton*>(widget) || qobject_cast<QCheckBox*>(widget) ||
        qobject_cast<QTabBar*>(widget);
    if (belongs_to_window && changes_value) {
      auto* wheel = static_cast<QWheelEvent*>(event);
      QAbstractScrollArea* scroll_area = nullptr;
      for (QWidget* parent = widget->parentWidget(); parent && !scroll_area; parent = parent->parentWidget()) {
        scroll_area = qobject_cast<QAbstractScrollArea*>(parent);
        if (parent == root)
          break;
      }
      if (scroll_area) {
        // Let the viewport interpret pixels, rows and platform scroll settings.
        // Its event is not a value-control event, so it bypasses this guard.
        QWidget* viewport = scroll_area->viewport();
        QWheelEvent forwarded(
            viewport->mapFromGlobal(wheel->globalPosition().toPoint()),
            wheel->globalPosition(),
            wheel->pixelDelta(),
            wheel->angleDelta(),
            wheel->buttons(),
            wheel->modifiers(),
            wheel->phase(),
            wheel->inverted(),
            wheel->source());
        QCoreApplication::sendEvent(viewport, &forwarded);
      }
      wheel->accept();
      return true;
    }
  }
  return false;
}
