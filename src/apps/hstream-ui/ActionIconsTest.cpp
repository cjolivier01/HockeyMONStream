#include "src/apps/hstream-ui/ActionIcons.h"

#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOption>

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void require_colored(const QImage& image, const QString& context) {
  int visible = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor color = image.pixelColor(x, y);
      if (color.alpha() < 24)
        continue;
      ++visible;
      const int high = std::max({color.red(), color.green(), color.blue()});
      const int low = std::min({color.red(), color.green(), color.blue()});
      if (high - low < 12)
        throw std::runtime_error((context + ": monochrome icon pixel").toStdString());
    }
  }
  if (!visible)
    throw std::runtime_error((context + ": empty icon").toStdString());
}

void check_icon(const QIcon& icon, const QString& context) {
  for (auto mode : {QIcon::Normal, QIcon::Active, QIcon::Selected, QIcon::Disabled}) {
    for (auto state : {QIcon::Off, QIcon::On}) {
      for (qreal ratio : {1.0, 2.0})
        require_colored(icon.pixmap(QSize(16, 16), ratio, mode, state).toImage(), context);
    }
  }
}

void check_control_glyph(QWidget& widget, QRect area) {
  widget.show();
  QApplication::processEvents();
  const QPixmap rendered = widget.grab();
  const qreal ratio = rendered.devicePixelRatio();
  area.adjust(3, 2, -3, -2); // Exclude the control's frame, keeping the central glyph.
  const QImage image = rendered.toImage().copy(QRect(area.topLeft() * ratio, area.size() * ratio));
  int colored = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor color = image.pixelColor(x, y);
      const int high = std::max({color.red(), color.green(), color.blue()});
      const int low = std::min({color.red(), color.green(), color.blue()});
      if (high - low >= 12)
        ++colored;
      else if (high < 100)
        throw std::runtime_error("Rendered complex control has a black glyph");
    }
  }
  if (!colored)
    throw std::runtime_error("Rendered complex control has no colored glyph");
}
} // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  try {
    // Directly constructed dialogs must also get colored icons without main().
    for (int value = 0; value <= static_cast<int>(ActionIcon::Undo); ++value)
      check_icon(action_icon(static_cast<ActionIcon>(value)), QString("direct action %1").arg(value));
    install_button_icon_style();
    QStyle* installed = application.style();
    install_button_icon_style();
    if (installed != application.style())
      throw std::runtime_error("Icon style must be installed only once");
    for (bool dark : {false, true}) {
      QPalette palette = application.palette();
      palette.setColor(QPalette::ButtonText, dark ? Qt::white : Qt::black);
      palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
      palette.setColor(QPalette::Button, dark ? QColor("#202a38") : QColor("#eeeeee"));
      application.setPalette(palette);
      for (int value = 0; value <= static_cast<int>(ActionIcon::Undo); ++value)
        check_icon(action_icon(static_cast<ActionIcon>(value)), QString("action %1").arg(value));
      for (int value = QStyle::SP_TitleBarMenuButton; value <= static_cast<int>(QStyle::SP_TabCloseButton); ++value) {
        const auto standard = static_cast<QStyle::StandardPixmap>(value);
        const QIcon icon = application.style()->standardIcon(standard);
        // Some platform styles intentionally omit unused standard glyphs.
        if (!icon.isNull())
          check_icon(icon, QString("standard icon %1").arg(value));
        const QPixmap pixmap = application.style()->standardPixmap(standard);
        if (!pixmap.isNull())
          require_colored(pixmap.toImage(), QString("standard pixmap %1").arg(value));
      }
      for (bool enabled : {false, true}) {
        for (auto arrow :
             {QStyle::PE_IndicatorArrowUp,
              QStyle::PE_IndicatorArrowDown,
              QStyle::PE_IndicatorArrowLeft,
              QStyle::PE_IndicatorArrowRight}) {
          QImage image(20, 20, QImage::Format_ARGB32_Premultiplied);
          image.fill(Qt::transparent);
          QPainter painter(&image);
          QStyleOption option;
          option.rect = image.rect();
          option.palette = palette;
          if (enabled)
            option.state |= QStyle::State_Enabled;
          application.style()->drawPrimitive(arrow, &option, &painter);
          painter.end();
          require_colored(image, "native navigation arrow");
        }
        for (auto glyph :
             {QStyle::PE_IndicatorSpinUp,
              QStyle::PE_IndicatorSpinDown,
              QStyle::PE_IndicatorSpinPlus,
              QStyle::PE_IndicatorSpinMinus,
              QStyle::PE_IndicatorHeaderArrow,
              QStyle::PE_IndicatorBranch}) {
          for (bool expanded : {false, true}) {
            QImage image(20, 20, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            QStyleOptionHeader option;
            option.rect = image.rect();
            option.palette = palette;
            option.sortIndicator = expanded ? QStyleOptionHeader::SortUp : QStyleOptionHeader::SortDown;
            option.state |= QStyle::State_Children;
            if (expanded)
              option.state |= QStyle::State_Open;
            if (enabled)
              option.state |= QStyle::State_Enabled;
            application.style()->drawPrimitive(glyph, &option, &painter);
            painter.end();
            require_colored(image, QString("native control indicator %1").arg(glyph));
          }
        }
      }
    }
    application.setPalette(application.style()->standardPalette());
    QSpinBox spin;
    spin.resize(120, 32);
    spin.setRange(0, 10);
    spin.setValue(5);
    QStyleOptionSpinBox spin_option;
    spin_option.initFrom(&spin);
    spin_option.frame = true;
    spin_option.stepEnabled = QAbstractSpinBox::StepUpEnabled | QAbstractSpinBox::StepDownEnabled;
    check_control_glyph(
        spin, application.style()->subControlRect(QStyle::CC_SpinBox, &spin_option, QStyle::SC_SpinBoxUp, &spin));
    QComboBox combo;
    combo.resize(120, 32);
    combo.addItem("Choice");
    QStyleOptionComboBox combo_option;
    combo_option.initFrom(&combo);
    check_control_glyph(
        combo,
        application.style()->subControlRect(QStyle::CC_ComboBox, &combo_option, QStyle::SC_ComboBoxArrow, &combo));
    QScrollBar scroll(Qt::Horizontal);
    scroll.resize(120, 20);
    scroll.setValue(50);
    QStyleOptionSlider scroll_option;
    scroll_option.initFrom(&scroll);
    scroll_option.orientation = Qt::Horizontal;
    check_control_glyph(
        scroll,
        application.style()->subControlRect(
            QStyle::CC_ScrollBar, &scroll_option, QStyle::SC_ScrollBarAddLine, &scroll));
    const QString gallery = qEnvironmentVariable("HSTREAM_TEST_ICON_GALLERY");
    if (!gallery.isEmpty()) {
      QImage image(640, 120, QImage::Format_RGB32);
      QPainter painter(&image);
      painter.fillRect(0, 0, 640, 60, QColor("#eeeeee"));
      painter.fillRect(0, 60, 640, 60, QColor("#202a38"));
      for (int value = 0; value <= static_cast<int>(ActionIcon::Undo); ++value) {
        const QIcon icon = action_icon(static_cast<ActionIcon>(value));
        for (int row = 0; row < 4; ++row)
          icon.paint(
              &painter, value * 20, row * 30 + 6, 18, 18, Qt::AlignCenter, row % 2 ? QIcon::Disabled : QIcon::Normal);
      }
      painter.end();
      if (!image.save(gallery))
        throw std::runtime_error("Cannot save icon gallery");
    }
    std::cout << "Colored action icons passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
