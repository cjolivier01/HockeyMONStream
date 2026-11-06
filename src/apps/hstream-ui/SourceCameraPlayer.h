#pragma once

#include <QtCore/QStringList>
#include <memory>

namespace hm::ui {
// One camera's original, untrimmed chapter timeline. Owns at most one hardware
// decoder; Close synchronously retires the graph before another owner starts.
class SourceCameraPlayer {
 public:
  struct Status {
    bool open{false};
    bool playing{false};
    bool ready{false};
    qint64 position_ms{0};
    qint64 presented_ns{-1};
    qint64 duration_ms{0};
    int width{0}, height{0};
    QString error;
  };
  SourceCameraPlayer();
  ~SourceCameraPlayer();
  bool Open(const QStringList& files, quint64 window, qint64 timeout_ms, QString* error);
  void Play();
  void Pause();
  // Repaint the retained GPU frame after a paused target was remapped/resized.
  bool Redraw();
  void Seek(qint64 position_ms);
  void Close();
  Status Poll();
  Status GetStatus() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace hm::ui
