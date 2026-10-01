#pragma once

#include <QtGui/QImage>
#include <QtWidgets/QWidget>

class QThread;

// Optional idle overlay. An empty source immediately drops the image and makes
// any outstanding decode obsolete, without waiting for it on the UI thread.
class StitchingStillPreview : public QWidget {
 public:
  explicit StitchingStillPreview(QWidget* parent);
  void setSource(
      const QString& directory,
      const QString& expected_generation = {},
      const QByteArray& expected_settings = {});

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void paintEvent(QPaintEvent*) override;

 private:
  void refresh();
  QString directory_;
  QString expected_generation_;
  QByteArray expected_settings_;
  QByteArray revision_;
  QImage image_;
  QThread* worker_{nullptr};
  quint64 request_{0};
};
