#pragma once

#include <QtCore/QObject>

// Install the Qt logger before QApplication so plugin startup failures persist.
void install_qt_diagnostics();

class UiDiagnostics final : public QObject {
 public:
  explicit UiDiagnostics(QObject* parent = nullptr);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
};
