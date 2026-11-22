#pragma once

#include "src/apps/hstream-ui/SourceCameraPlayer.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <filesystem>

namespace hm::ui {

// The preview renderer can terminate its process on an unsafe device error.
// Keep its graph and GPU context behind the same guard used by Program playback.
class SourceCameraProcess {
 public:
  using Status = SourceCameraPlayer::Status;
  bool Open(
      const QStringList& files,
      quint64 window,
      int timeout_ms,
      int kill_timeout_ms,
      const std::filesystem::path& game,
      const std::string& token,
      int64_t grace_ms,
      int64_t poll_ms,
      QString* error);
  void Play();
  void Pause();
  void Seek(qint64 position);
  bool Redraw();
  Status Poll();
  Status GetStatus() const {
    return status_;
  }
  void Close();
  ~SourceCameraProcess();

 private:
  QProcess* process_{nullptr};
  QJsonObject pending_;
  Status status_;
  bool awaiting_reply_{false};
  bool retry_redraw_{false};
  QElapsedTimer reply_timer_;
  int timeout_ms_{0};
  int kill_timeout_ms_{0};
};

int RunSourceCameraProcess(const QStringList& arguments);

} // namespace hm::ui
