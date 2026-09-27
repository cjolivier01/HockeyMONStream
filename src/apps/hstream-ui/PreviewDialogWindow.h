#pragma once

#include "src/apps/hstream-ui/ActionIcons.h"

#include <QtCore/QEvent>
#include <QtWidgets/QDialog>
#include <QtWidgets/QToolButton>

// Qt::Window belongs in the QDialog constructor. Native Dialog windows cannot
// maximize under Mutter even when they request a maximize button.
inline void configure_preview_dialog_window(QDialog* dialog) {
  dialog->setWindowFlag(Qt::WindowMaximizeButtonHint, true);
  dialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  dialog->setSizeGripEnabled(true);
}

// Keep the explicit window action in sync with title-bar and window-manager changes.
class PreviewDialogWindowSizeButton : public QToolButton {
 public:
  explicit PreviewDialogWindowSizeButton(QDialog* dialog) : QToolButton(dialog), dialog_(dialog) {
    dialog_->installEventFilter(this);
    connect(this, &QToolButton::clicked, dialog_, [this]() {
      if (dialog_->isMaximized())
        dialog_->showNormal();
      else
        dialog_->showMaximized();
    });
    updateAction();
  }

 protected:
  bool eventFilter(QObject* object, QEvent* event) override {
    if (object == dialog_ && event->type() == QEvent::WindowStateChange)
      updateAction();
    return QToolButton::eventFilter(object, event);
  }

 private:
  void updateAction() {
    const bool maximized = dialog_->isMaximized();
    setIcon(action_icon(maximized ? ActionIcon::Restore : ActionIcon::Expand));
    setText(maximized ? "Restore window" : "Maximize window");
    setToolTip(text());
    setAccessibleName(text());
  }

  QDialog* dialog_;
};

class PreviewFocusButton : public QToolButton {
 public:
  explicit PreviewFocusButton(QWidget* parent) : QToolButton(parent) {
    setToolButtonStyle(Qt::ToolButtonIconOnly);
    setIconSize(QSize(20, 20));
    setFixedSize(32, 32);
    setFocused(false);
  }

  void setFocused(bool focused) {
    setIcon(action_icon(focused ? ActionIcon::Restore : ActionIcon::Expand));
    const QString label = focused ? "Restore preview" : "Maximize preview";
    setToolTip(label + " (or double-click the preview)");
    setAccessibleName(label);
  }
};
