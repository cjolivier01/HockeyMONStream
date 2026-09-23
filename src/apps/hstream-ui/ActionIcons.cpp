#include "src/apps/hstream-ui/ActionIcons.h"

#include <QtCore/QVariant>
#include <QtGui/QPainter>
#include <QtGui/QPalette>
#include <QtGui/QPixmap>
#include <QtGui/QPolygonF>
#include <QtWidgets/QApplication>
#include <QtWidgets/QProxyStyle>
#include <QtWidgets/QStyle>

namespace {
class ButtonIconStyle : public QProxyStyle {
 public:
  explicit ButtonIconStyle(QStyle* base) : QProxyStyle(base) {}
  int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget, QStyleHintReturn* data)
      const override {
    if (hint == SH_DialogButtonBox_ButtonsHaveIcons)
      return 1;
    return QProxyStyle::styleHint(hint, option, widget, data);
  }
};

QIcon drawn_icon(ActionIcon action) {
  QIcon icon;
  for (int size : {16, 24, 32, 48, 64}) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 24.0, size / 24.0);
    p.setPen(QPen(qApp->palette().color(QPalette::ButtonText), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
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
    icon.addPixmap(pixmap);
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
  return qApp->style()->standardIcon(standard);
}
