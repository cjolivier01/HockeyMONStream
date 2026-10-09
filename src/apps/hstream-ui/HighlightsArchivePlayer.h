#pragma once

#include <QtCore/QString>
#include <QtCore/QVector>

#include <memory>

namespace hm::ui {

// Plays selected ranges of an already-published archive into a native window
// using the GPU preview sink. One decode graph, no intermediate files and no
// subprocess: previewing a highlight is a seek, not a pipeline run.
class HighlightsArchivePlayer {
 public:
  struct Segment {
    qint64 start_ms{0};
    qint64 end_ms{0};
  };
  struct Status {
    bool playing{false};
    bool finished{false};
    // Index into the segment list currently being played, or -1.
    int segment{-1};
    qint64 position_ms{-1};
    QString error;
  };

  HighlightsArchivePlayer();
  ~HighlightsArchivePlayer();
  HighlightsArchivePlayer(const HighlightsArchivePlayer&) = delete;
  HighlightsArchivePlayer& operator=(const HighlightsArchivePlayer&) = delete;

  // True when this build and display can host the GPU renderer at all.
  static bool Available();

  // width and height are the archive's native size; they only decide how far
  // the preview is scaled down before it reaches the renderer. Pass 0 to send
  // frames through unscaled.
  bool Open(const QString& archive_path, quint64 window_id, int width, int height, QString* error);
  bool Play(const QVector<Segment>& segments, bool loop, QString* error);
  void Stop();
  void Close();
  // Pumps the GStreamer bus and reports progress. Call from a Qt timer.
  Status Poll();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace hm::ui
