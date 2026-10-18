#pragma once

#include <algorithm>
#include <cmath>

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
        const QPoint pixels = wheel->pixelDelta();
        const QPoint angles = wheel->angleDelta();
        const bool horizontal = std::abs(pixels.x()) > std::abs(pixels.y()) ||
            (pixels.isNull() && std::abs(angles.x()) > std::abs(angles.y()));
        QScrollBar* bar = horizontal ? scroll_area->horizontalScrollBar() : scroll_area->verticalScrollBar();
        const int raw_delta =
            horizontal ? (pixels.x() != 0 ? pixels.x() : angles.x()) : (pixels.y() != 0 ? pixels.y() : angles.y());
        if (bar && raw_delta != 0) {
          const bool pixel_scroll = horizontal ? pixels.x() != 0 : pixels.y() != 0;
          const int delta = pixel_scroll ? raw_delta : raw_delta * std::max(1, bar->singleStep()) / 40;
          bar->setValue(bar->value() - delta);
        }
      }
      wheel->accept();
      return true;
    }
  }
  return false;
}
