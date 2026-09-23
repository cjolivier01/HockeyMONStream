#include "src/apps/hstream-ui/ActionIcons.h"

#include <QtCore/QScopedValueRollback>
#include <QtCore/QVariant>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPalette>
#include <QtGui/QPixmap>
#include <QtGui/QPolygonF>
#include <QtWidgets/QApplication>
#include <QtWidgets/QProxyStyle>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOption>

#include <algorithm>
#include <array>

namespace {
QColor standard_color(QStyle::StandardPixmap icon) {
  using S = QStyle;
  switch (icon) {
    case S::SP_MediaPlay:
    case S::SP_DialogApplyButton:
    case S::SP_DialogOkButton:
    case S::SP_DialogYesButton:
    case S::SP_DialogYesToAllButton:
    case S::SP_FileDialogNewFolder:
      return QColor("#229954");
    case S::SP_TrashIcon:
    case S::SP_MediaStop:
    case S::SP_BrowserStop:
    case S::SP_DialogCancelButton:
    case S::SP_DialogCloseButton:
    case S::SP_DialogDiscardButton:
    case S::SP_DialogAbortButton:
    case S::SP_DialogNoButton:
    case S::SP_DialogNoToAllButton:
    case S::SP_TitleBarCloseButton:
    case S::SP_DockWidgetCloseButton:
    case S::SP_TabCloseButton:
    case S::SP_MessageBoxCritical:
      return QColor("#d94a53");
    case S::SP_MediaPause:
    case S::SP_MessageBoxWarning:
    case S::SP_DirIcon:
    case S::SP_DirOpenIcon:
    case S::SP_DirClosedIcon:
    case S::SP_DialogOpenButton:
      return QColor("#c48920");
    case S::SP_BrowserReload:
    case S::SP_DialogResetButton:
    case S::SP_DialogRetryButton:
    case S::SP_RestoreDefaultsButton:
      return QColor("#159895");
    default:
      return QColor("#2d7dd2");
  }
}

QPixmap tinted_pixmap(const QPixmap& source, const QColor& color) {
  if (source.isNull())
    return source;
  QImage image = source.toImage().convertToFormat(QImage::Format_ARGB32);
  // Retain shading and highlights inside platform glyphs. A flat alpha tint
  // would erase details such as the cutout in a save icon. Even the darkest
  // source pixel becomes the semantic color instead of black.
  std::array<QRgb, 256> shades;
  for (int gray = 0; gray < 256; ++gray) {
    const double highlight = gray / 255.0 * 0.72;
    shades[gray] = qRgb(
        color.red() + (255 - color.red()) * highlight,
        color.green() + (255 - color.green()) * highlight,
        color.blue() + (255 - color.blue()) * highlight);
  }
  for (int y = 0; y < image.height(); ++y) {
    auto* pixels = reinterpret_cast<QRgb*>(image.scanLine(y));
    for (int x = 0; x < image.width(); ++x) {
      const QRgb shade = shades[qGray(pixels[x])];
      pixels[x] = qRgba(qRed(shade), qGreen(shade), qBlue(shade), qAlpha(pixels[x]));
    }
  }
  QPixmap result = QPixmap::fromImage(image);
  result.setDevicePixelRatio(source.devicePixelRatio());
  return result;
}

void add_colored_modes(QIcon& icon, const QPixmap& normal, QIcon::State state) {
  if (normal.isNull())
    return;
  QPixmap disabled = normal;
  QPainter fade(&disabled);
  fade.setCompositionMode(QPainter::CompositionMode_DestinationIn);
  fade.fillRect(disabled.rect(), QColor(255, 255, 255, 115));
  fade.end();
  // Explicit variants prevent Qt's default disabled-icon grayscale conversion.
  for (auto mode : {QIcon::Normal, QIcon::Active, QIcon::Selected})
    icon.addPixmap(normal, mode, state);
  icon.addPixmap(disabled, QIcon::Disabled, state);
}

QIcon colored_icon(const QIcon& source, const QColor& color) {
  QIcon result;
  for (int size : {16, 24, 32, 48, 64}) {
    for (auto state : {QIcon::Off, QIcon::On})
      add_colored_modes(result, tinted_pixmap(source.pixmap(size, size, QIcon::Normal, state), color), state);
  }
  return result;
}

class ButtonIconStyle : public QProxyStyle {
 public:
  explicit ButtonIconStyle(QStyle* base) : QProxyStyle(base) {}
  int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget, QStyleHintReturn* data)
      const override {
    if (hint == SH_DialogButtonBox_ButtonsHaveIcons)
      return 1;
    return QProxyStyle::styleHint(hint, option, widget, data);
  }
  QIcon standardIcon(StandardPixmap icon, const QStyleOption* option, const QWidget* widget) const override {
    if (generating_icon_)
      return QProxyStyle::standardIcon(icon, option, widget);
    QScopedValueRollback<bool> guard(generating_icon_, true);
    return colored_icon(QProxyStyle::standardIcon(icon, option, widget), standard_color(icon));
  }
  QPixmap standardPixmap(StandardPixmap icon, const QStyleOption* option, const QWidget* widget) const override {
    if (generating_icon_)
      return QProxyStyle::standardPixmap(icon, option, widget);
    QScopedValueRollback<bool> guard(generating_icon_, true);
    return tinted_pixmap(QProxyStyle::standardPixmap(icon, option, widget), standard_color(icon));
  }
  void drawComplexControl(
      ComplexControl control,
      const QStyleOptionComplex* option,
      QPainter* painter,
      const QWidget* widget) const override {
    // Fusion paints some arrows directly inside complex controls instead of
    // delegating to drawPrimitive. Color just their subcontrols; text and frames
    // keep the platform palette.
    if (control == CC_SpinBox) {
      if (const auto* spin = qstyleoption_cast<const QStyleOptionSpinBox*>(option)) {
        draw_colored_subcontrols(control, *spin, SC_SpinBoxUp | SC_SpinBoxDown, painter, widget);
        return;
      }
    } else if (control == CC_ComboBox) {
      if (const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option)) {
        draw_colored_subcontrols(control, *combo, SC_ComboBoxArrow, painter, widget);
        return;
      }
    } else if (control == CC_ScrollBar) {
      if (const auto* slider = qstyleoption_cast<const QStyleOptionSlider*>(option)) {
        draw_colored_subcontrols(control, *slider, SC_ScrollBarAddLine | SC_ScrollBarSubLine, painter, widget);
        return;
      }
    }
    QProxyStyle::drawComplexControl(control, option, painter, widget);
  }
  void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget)
      const override {
    const bool navigation = element >= PE_IndicatorArrowDown && element <= PE_IndicatorArrowUp;
    const bool spin = element == PE_IndicatorSpinUp || element == PE_IndicatorSpinDown ||
        element == PE_IndicatorSpinPlus || element == PE_IndicatorSpinMinus;
    if (!painting_indicator_ && option && !option->rect.isEmpty() &&
        (navigation || spin || element == PE_IndicatorHeaderArrow || element == PE_IndicatorBranch)) {
      // Preserve the platform's glyph shape, sort direction and tree expansion
      // state. These tiny UI indicators are painted only on widget invalidation.
      const qreal ratio = painter->device()->devicePixelRatioF();
      QPixmap glyph(option->rect.size() * ratio);
      glyph.setDevicePixelRatio(ratio);
      glyph.fill(Qt::transparent);
      QPainter native(&glyph);
      native.translate(-option->rect.topLeft());
      {
        QScopedValueRollback<bool> guard(painting_indicator_, true);
        QProxyStyle::drawPrimitive(element, option, &native, widget);
      }
      native.end();
      painter->save();
      if (!option->state.testFlag(State_Enabled))
        painter->setOpacity(painter->opacity() * 0.45);
      painter->drawPixmap(option->rect.topLeft(), tinted_pixmap(glyph, QColor("#2d7dd2")));
      painter->restore();
      return;
    }
    QProxyStyle::drawPrimitive(element, option, painter, widget);
  }

 private:
  template <typename Option>
  void draw_colored_subcontrols(
      ComplexControl control,
      const Option& option,
      SubControls glyphs,
      QPainter* painter,
      const QWidget* widget) const {
    Option background(option);
    background.subControls &= ~glyphs;
    QProxyStyle::drawComplexControl(control, &background, painter, widget);
    Option arrows(option);
    arrows.subControls &= glyphs;
    const QColor blue("#2d7dd2");
    QColor disabled = blue;
    disabled.setAlpha(115);
    for (auto role : {QPalette::ButtonText, QPalette::WindowText, QPalette::Text}) {
      arrows.palette.setColor(QPalette::All, role, blue);
      arrows.palette.setColor(QPalette::Disabled, role, disabled);
    }
    QProxyStyle::drawComplexControl(control, &arrows, painter, widget);
  }

  // Platform styles can implement standardIcon via proxy()->standardPixmap.
  // Tint only the final result so nested lookups retain their original contrast.
  mutable bool generating_icon_{false};
  mutable bool painting_indicator_{false};
};

QIcon drawn_icon(ActionIcon action) {
  QIcon icon;
  for (int size : {16, 24, 32, 48, 64}) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 24.0, size / 24.0);
    const QColor color = action == ActionIcon::Add                       ? QColor("#229954")
        : action == ActionIcon::Remove                                   ? QColor("#c48920")
        : action == ActionIcon::Camera                                   ? QColor("#9966dd")
        : action == ActionIcon::Stitching || action == ActionIcon::Level ? QColor("#159895")
        : action == ActionIcon::Crop                                     ? QColor("#c48920")
                                                                         : QColor("#2d7dd2");
    p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (action == ActionIcon::Add || action == ActionIcon::Remove) {
      p.drawLine(QPointF(4, 12), QPointF(20, 12));
      if (action == ActionIcon::Add)
        p.drawLine(QPointF(12, 4), QPointF(12, 20));
    } else if (action == ActionIcon::ActualSize) {
      p.drawRect(QRectF(2, 4, 20, 16));
      QFont font = p.font();
      font.setPixelSize(10);
      p.setFont(font);
      p.drawText(QRectF(2, 4, 20, 16), Qt::AlignCenter, "1:1");
    } else if (action == ActionIcon::Camera) {
      p.drawRoundedRect(QRectF(2, 7, 20, 14), 2, 2);
      p.drawEllipse(QPointF(12, 14), 4, 4);
      p.drawPolyline(QPolygonF({{6, 7}, {8, 3}, {16, 3}, {18, 7}}));
    } else if (action == ActionIcon::Stitching) {
      p.drawRoundedRect(QRectF(2, 4, 12, 16), 1, 1);
      p.drawRoundedRect(QRectF(10, 4, 12, 16), 1, 1);
      p.drawPolyline(QPolygonF({{3, 16}, {7, 11}, {12, 15}, {17, 10}, {21, 16}}));
    } else if (action == ActionIcon::Crop) {
      p.drawPolyline(QPolygonF({{7, 2}, {7, 17}, {22, 17}}));
      p.drawPolyline(QPolygonF({{2, 7}, {17, 7}, {17, 22}}));
    } else if (action == ActionIcon::Level) {
      p.drawRoundedRect(QRectF(2, 8, 20, 8), 1, 1);
      p.drawEllipse(QPointF(12, 12), 2, 2);
      p.drawLine(QPointF(7, 8), QPointF(7, 16));
      p.drawLine(QPointF(17, 8), QPointF(17, 16));
    } else {
      p.drawEllipse(QPointF(10, 10), 7, 7);
      p.drawLine(QPointF(15, 15), QPointF(22, 22));
      p.drawLine(QPointF(6, 10), QPointF(14, 10));
      if (action == ActionIcon::ZoomIn)
        p.drawLine(QPointF(10, 6), QPointF(10, 14));
    }
    p.end();
    for (auto state : {QIcon::Off, QIcon::On})
      add_colored_modes(icon, pixmap, state);
  }
  return icon;
}
} // namespace

void install_button_icon_style() {
  if (!qApp->property("hstreamButtonIconStyle").toBool()) {
    qApp->setProperty("hstreamButtonIconStyle", true);
    // QProxyStyle takes ownership of the current style; QApplication owns the
    // proxy. Keep the platform style instead of selecting a different theme.
    QApplication::setStyle(new ButtonIconStyle(QApplication::style()));
  }
}

QIcon action_icon(ActionIcon action) {
  using S = QStyle;
  switch (action) {
    case ActionIcon::Add:
    case ActionIcon::Remove:
    case ActionIcon::ActualSize:
    case ActionIcon::Camera:
    case ActionIcon::Stitching:
    case ActionIcon::Crop:
    case ActionIcon::Level:
    case ActionIcon::ZoomIn:
    case ActionIcon::ZoomOut:
      return drawn_icon(action);
    default:
      break;
  }
  S::StandardPixmap standard = S::SP_FileIcon;
  switch (action) {
    case ActionIcon::Delete:
      standard = S::SP_TrashIcon;
      break;
    case ActionIcon::Open:
      standard = S::SP_DirOpenIcon;
      break;
    case ActionIcon::Save:
      standard = S::SP_DialogSaveButton;
      break;
    case ActionIcon::Apply:
      standard = S::SP_DialogApplyButton;
      break;
    case ActionIcon::Cancel:
      standard = S::SP_DialogCancelButton;
      break;
    case ActionIcon::Close:
      standard = S::SP_DialogCloseButton;
      break;
    case ActionIcon::Reset:
      standard = S::SP_DialogResetButton;
      break;
    case ActionIcon::Refresh:
      standard = S::SP_BrowserReload;
      break;
    case ActionIcon::Play:
      standard = S::SP_MediaPlay;
      break;
    case ActionIcon::Pause:
      standard = S::SP_MediaPause;
      break;
    case ActionIcon::Stop:
      standard = S::SP_MediaStop;
      break;
    case ActionIcon::Previous:
      standard = S::SP_MediaSeekBackward;
      break;
    case ActionIcon::Next:
      standard = S::SP_MediaSeekForward;
      break;
    case ActionIcon::Expand:
    case ActionIcon::Fit:
      standard = S::SP_TitleBarMaxButton;
      break;
    case ActionIcon::Restore:
      standard = S::SP_TitleBarNormalButton;
      break;
    case ActionIcon::Prepare:
      standard = S::SP_ComputerIcon;
      break;
    case ActionIcon::Inspect:
      standard = S::SP_FileDialogContentsView;
      break;
    case ActionIcon::Document:
      standard = S::SP_FileIcon;
      break;
    case ActionIcon::Network:
      standard = S::SP_DriveNetIcon;
      break;
    case ActionIcon::Undo:
      standard = S::SP_ArrowBack;
      break;
    default:
      break;
  }
  // Also support direct dialog construction in embedders/tests without the
  // application-wide proxy installed by main().
  const QIcon icon = qApp->style()->standardIcon(standard);
  return qApp->property("hstreamButtonIconStyle").toBool() ? icon : colored_icon(icon, standard_color(standard));
}
