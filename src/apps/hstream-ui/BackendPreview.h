#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtGui/QImage>

#include <functional>

namespace hm::ui {

// Bounded compatibility transport for a Python worker in another process.
// Native HStream previews continue to render directly into their X11 surfaces.
class BackendPreview : public QObject {
 public:
  explicit BackendPreview(QObject* parent = nullptr);
  void start(const QString& directory);
  void stop();
  void setChannel(const QString& channel);
  void poll();
  static QString fileName(const QString& channel);
  static QImage readImage(const QString& path);

  std::function<void(const QString&, const QImage&)> frameReady;
  std::function<void(const QString&)> failed;

 private:
  QTimer timer_;
  QString directory_;
  QString channel_;
  qint64 previous_modified_{-1};
  qint64 previous_size_{-1};
};

} // namespace hm::ui
