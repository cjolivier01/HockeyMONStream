#pragma once

#include <QtGui/QImage>
#include <QtWidgets/QWidget>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

class QThread;

// Optional idle overlay. An empty source immediately drops the image and makes
// any outstanding decode obsolete, without waiting for it on the UI thread.
// Source changes drain only this reader's metadata lock section, so its own
// obsolete read cannot make a subsequent Play/config save appear writer-busy.
class StitchingStillPreview : public QWidget {
 public:
  explicit StitchingStillPreview(QWidget* parent);
  ~StitchingStillPreview() override;
  void setSource(
      const QString& directory,
      const QString& expected_generation = {},
      const QByteArray& expected_settings = {});

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void paintEvent(QPaintEvent*) override;

 private:
  friend struct StitchingStillPreviewTestAccess;
  struct ReadState {
    std::mutex metadata_mutex;
    std::atomic<bool> cancelled{false};
  };
  void cancelRead();
  void refresh();
  QString directory_;
  QString expected_generation_;
  QByteArray expected_settings_;
  QByteArray revision_;
  QImage image_;
  QThread* worker_{nullptr};
  std::shared_ptr<ReadState> read_state_;
  // Private phase observers let interaction tests hold actual metadata/decode
  // work at deterministic boundaries; normal instances leave them empty.
  std::function<void()> metadata_locked_observer_;
  std::function<void()> decoding_observer_;
  quint64 request_{0};
};
