#include "src/apps/hstream-ui/UiDiagnostics.h"

#include <cstdio>

#include <QtCore/QEvent>
#include <QtCore/QLoggingCategory>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include "hstream/src/libs/common/ProcessDiagnostics.h"

namespace {
QtMessageHandler previous_handler = nullptr;
void qt_message(QtMsgType type, const QMessageLogContext& context, const QString& message) {
  const auto bytes = qFormatLogMessage(type, context, message).toUtf8();
  hm::diagnostics::Log(
      type == QtWarningMsg ? "qt-warning" : (type == QtCriticalMsg || type == QtFatalMsg ? "qt-error" : "qt"),
      {bytes.constData(), static_cast<size_t>(bytes.size())});
  if (type == QtCriticalMsg || type == QtFatalMsg)
    hm::diagnostics::Breadcrumb("qt-error", {bytes.constData(), static_cast<size_t>(bytes.size())});
  if (previous_handler)
    previous_handler(type, context, message);
  else {
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stderr);
    std::fputc('\n', stderr);
  }
}
} // namespace

void install_qt_diagnostics() {
  previous_handler = qInstallMessageHandler(qt_message);
}

UiDiagnostics::UiDiagnostics(QObject* parent) : QObject(parent) {
  qApp->installEventFilter(this);
}

bool UiDiagnostics::eventFilter(QObject* watched, QEvent* event) {
  const auto type = event->type();
  if (type != QEvent::MouseButtonPress && type != QEvent::KeyPress && type != QEvent::Show && type != QEvent::Hide &&
      type != QEvent::Close && type != QEvent::WindowStateChange)
    return false;
  auto* widget = qobject_cast<QWidget*>(watched);
  if (!widget)
    return false;
  QString detail;
  if (type == QEvent::MouseButtonPress || type == QEvent::KeyPress) {
    auto* button = qobject_cast<QAbstractButton*>(widget);
    if (!button)
      return false;
    if (type == QEvent::KeyPress) {
      const auto key = static_cast<QKeyEvent*>(event)->key();
      if (key != Qt::Key_Enter && key != Qt::Key_Return && key != Qt::Key_Space)
        return false; // Never record typed input.
    }
    detail = QString("button=%1 name=%2 window=%3")
                 .arg(button->text(), button->objectName(), widget->window()->windowTitle());
  } else {
    if (!widget->isWindow())
      return false;
    detail = QString("window=%1 event=%2 state=%3")
                 .arg(widget->windowTitle())
                 .arg(int(type))
                 .arg(int(widget->windowState()));
  }
  const auto bytes = detail.toUtf8();
  hm::diagnostics::Breadcrumb("ui", {bytes.constData(), static_cast<size_t>(bytes.size())});
  return false;
}
