#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace hm::ui {

// The desktop owns workflow editors. This selection changes only the Program
// runner; calibration and offline workflows keep the shared native engines.
struct BackendOptions {
  QString name{"hstream"};
  QString runner;
  QString initial_launch;

  bool isHockeyMon() const {
    return name == "hockeymon";
  }
  void validate() const;
  void verifyRunner() const;
  static void validateHandshake(const QJsonObject& handshake);
};

} // namespace hm::ui
