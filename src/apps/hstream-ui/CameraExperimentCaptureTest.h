#pragma once

// Private Camera test-driver protocol. This is linked only into the test binary.
#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>
#include <QtCore/QString>

#include <functional>
#include <map>

namespace camera_capture_test {

struct Controls {
  std::map<QString, double> values;
  qint64 playback_start_ms{0};
  bool complete{false};
};

Controls parse_controls(const QByteArray& bytes);
Controls read_controls(const QString& path);
void write_json(const QString& path, const QJsonObject& value);
int confirmed_frame(const QString& label, int frame_count, bool screenshot_ready);
QString seek_phase(int frame, int sequence);
QJsonObject capture_identity(QJsonObject value);

class Handshake {
 public:
  explicit Handshake(const QString& directory);
  ~Handshake();
  Handshake(const Handshake&) = delete;
  Handshake& operator=(const Handshake&) = delete;
  bool enabled() const {
    return descriptor_ >= 0;
  }
  void begin(const QString& phase, const QJsonObject& evidence);
  void checkpoint(
      const QString& phase,
      const QJsonObject& evidence,
      const std::function<QJsonObject()>& stable_state,
      int timeout_ms = 5000);

 private:
  void check_directory() const;
  QString directory_;
  QString token_;
  int descriptor_{-1};
  quint64 device_{0}, inode_{0};
  int sequence_{0};
  QString pending_phase_;
  QSet<QString> phases_;
};

bool tests();
} // namespace camera_capture_test
