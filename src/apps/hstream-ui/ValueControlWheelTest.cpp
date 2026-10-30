#include "src/apps/hstream-ui/ValueControlWheel.h"

#include <QtTest/qtest_widgets.h>
#include <QtGui/QWheelEvent>
#include <QtTest/QTest>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTimeEdit>
#include <QtWidgets/QVBoxLayout>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void wheel(QWidget* target, QPoint angles = {0, -120}, QPoint pixels = {}, Qt::KeyboardModifiers modifiers = {}) {
  const QPoint center = target->rect().center();
  QWheelEvent event(
      center, target->mapToGlobal(center), pixels, angles, Qt::NoButton, modifiers, Qt::ScrollUpdate, false);
  QApplication::sendEvent(target, &event);
}

struct Pane {
  QScrollArea* scroll;
  QWidget* content;
  QVBoxLayout* layout;
  explicit Pane(QWidget* parent, QSize size) : scroll(new QScrollArea(parent)), content(new QWidget) {
    content->setFixedSize(size);
    layout = new QVBoxLayout(content);
    scroll->setWidget(content);
  }
};

class ZoomCanvas : public QGraphicsView {
 protected:
  void wheelEvent(QWheelEvent* event) override {
    scale(1.25, 1.25);
    event->accept();
  }
};

void exercise() {
  // Neither this dialog nor its dynamically created controls have a main
  // window parent or a dialog-local filter.
  QDialog dialog;
  dialog.setWindowTitle("Wheel scrolling — independent dialog regression");
  auto* layout = new QVBoxLayout(&dialog);
  auto* report = new QLabel("Focused controls must keep their values", &dialog);
  report->setMinimumHeight(50);
  layout->addWidget(report);
  Pane outer(&dialog, {800, 1200});
  layout->addWidget(outer.scroll);
  Pane inner(outer.content, {700, 900});
  inner.scroll->setFixedSize(380, 180);
  outer.layout->addWidget(inner.scroll);
  outer.layout->addStretch();
  std::vector<std::pair<QWidget*, std::function<QString()>>> controls;
  auto add = [&](QWidget* widget, std::function<QString()> value) {
    inner.layout->addWidget(widget);
    controls.emplace_back(widget, value);
  };
  auto* combo = new QComboBox;
  combo->addItems({"First", "Second", "Third"});
  combo->setCurrentIndex(1);
  combo->setEditable(true);
  add(combo, [=] { return combo->currentText(); });
  controls.emplace_back(combo->lineEdit(), [=] { return combo->currentText(); });
  auto* spin = new QSpinBox;
  spin->setValue(50);
  add(spin, [=] { return spin->text(); });
  controls.emplace_back(spin->findChild<QLineEdit*>(), [=] { return spin->text(); });
  auto* decimal = new QDoubleSpinBox;
  decimal->setValue(4.5);
  add(decimal, [=] { return decimal->text(); });
  auto* time = new QTimeEdit(QTime(12, 30));
  add(time, [=] { return time->text(); });
  auto* slider = new QSlider(Qt::Horizontal);
  slider->setValue(50);
  add(slider, [=] { return QString::number(slider->value()); });
  auto* checkbox = new QCheckBox("Check");
  add(checkbox, [=] { return QString::number(checkbox->isChecked()); });
  auto* radio = new QRadioButton("Radio");
  add(radio, [=] { return QString::number(radio->isChecked()); });
  auto* text = new QLineEdit("Saved text");
  add(text, [=] { return text->text(); });
  auto* tabs = new QTabBar;
  tabs->addTab("One");
  tabs->addTab("Two");
  add(tabs, [=] { return QString::number(tabs->currentIndex()); });
  auto* child = new QLabel(combo);
  controls.emplace_back(child, [=] { return combo->currentText(); });
  dialog.resize(460, 300);
  dialog.show();
  QTest::qWait(20);
  for (const auto& [control, value] : controls) {
    for (bool focus : {false, true}) {
      if (focus)
        control->setFocus();
      else
        control->clearFocus();
      const auto before = value();
      for (Qt::KeyboardModifiers modifiers : {Qt::KeyboardModifiers{}, Qt::KeyboardModifiers{Qt::ControlModifier}}) {
        inner.scroll->verticalScrollBar()->setValue(50);
        outer.scroll->verticalScrollBar()->setValue(0);
        wheel(control, {0, -120}, {}, modifiers);
        if (control == combo && !focus && modifiers == Qt::NoModifier) {
          report->setText(QString("Wheel over dropdown: %1 → %2\nContaining pane scroll: 50 → %3")
                              .arg(before, value())
                              .arg(inner.scroll->verticalScrollBar()->value()));
          const auto screenshot = qEnvironmentVariable("HSTREAM_TEST_WHEEL_SCREENSHOT");
          if (!screenshot.isEmpty())
            dialog.grab().save(screenshot);
        }
        require(value() == before, "Wheel changed a control value");
        require(inner.scroll->verticalScrollBar()->value() > 50, "Wheel did not scroll the nearest pane");
        require(outer.scroll->verticalScrollBar()->value() == 0, "Wheel scrolled both panes");
      }
    }
  }
  inner.scroll->verticalScrollBar()->setValue(0);
  wheel(spin, {}, {0, -13});
  require(inner.scroll->verticalScrollBar()->value() == 13, "Pixel scrolling must be exact and installed only once");
  inner.scroll->horizontalScrollBar()->setValue(0);
  wheel(combo, {}, {-17, 0});
  require(inner.scroll->horizontalScrollBar()->value() == 17, "Horizontal trackpad input must scroll horizontally");
  wheel(combo, {0, -120}, {}, Qt::ShiftModifier);
  require(inner.scroll->horizontalScrollBar()->value() > 17, "Shift-wheel must scroll horizontally");

  inner.scroll->verticalScrollBar()->setValue(inner.scroll->verticalScrollBar()->maximum());
  outer.scroll->verticalScrollBar()->setValue(0);
  wheel(spin);
  require(
      outer.scroll->verticalScrollBar()->value() > 0, "An exhausted inner pane must hand scrolling to its ancestor");
  inner.scroll->verticalScrollBar()->setValue(0);
  outer.scroll->verticalScrollBar()->setValue(50);
  wheel(spin, {0, 120});
  require(outer.scroll->verticalScrollBar()->value() < 50, "Ancestor scrolling must work in both directions");
  // A control not inside a scroll pane remains immutable, even with focus.
  QComboBox isolated;
  isolated.addItems({"One", "Two", "Three"});
  isolated.setCurrentIndex(1);
  isolated.show();
  isolated.setFocus();
  wheel(&isolated);
  require(isolated.currentIndex() == 1, "No scroll pane must not enable wheel editing");
  QTest::keyClick(&isolated, Qt::Key_Down);
  require(isolated.currentIndex() == 2, "Explicit keyboard editing must still work");
  inner.scroll->ensureWidgetVisible(checkbox);
  QTest::mouseClick(checkbox, Qt::LeftButton, Qt::NoModifier, QPoint(8, checkbox->height() / 2));
  require(checkbox->isChecked(), "Explicit clicking must still work");

  QComboBox popup;
  for (int i = 0; i < 100; ++i)
    popup.addItem(QString::number(i));
  popup.setStyleSheet("QComboBox { combobox-popup: 0; }");
  popup.setMaxVisibleItems(5);
  popup.show();
  popup.showPopup();
  QTest::qWait(20);
  const auto before = popup.currentIndex();
  wheel(popup.view()->viewport());
  require(popup.currentIndex() == before, "Scrolling an open popup must not commit a selection");
  require(popup.view()->verticalScrollBar()->value() > 0, "Open popup contents must remain scrollable");
  popup.hidePopup();

  ZoomCanvas canvas;
  canvas.show();
  wheel(canvas.viewport());
  require(canvas.transform().m11() == 1.25, "Image canvas wheel zoom must remain available");
}
} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  install_value_control_wheel_guard();
  install_value_control_wheel_guard();
  try {
    exercise();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
