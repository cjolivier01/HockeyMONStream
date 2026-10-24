#include "src/apps/hstream-ui/BackendPreview.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtGui/QImageReader>

#include <stdexcept>

namespace hm::ui {

BackendPreview::BackendPreview(QObject* parent) : QObject(parent) {
  // Match the existing Python UI bridge's bounded preview cadence. No file
  // decoding or GPU readback is added to the native HStream renderer.
  timer_.setInterval(100);
  connect(&timer_, &QTimer::timeout, this, [this]() { poll(); });
}

void BackendPreview::start(const QString& directory) {
  stop();
  directory_ = directory;
  timer_.start();
}

void BackendPreview::stop() {
  timer_.stop();
  previous_modified_ = previous_size_ = -1;
  directory_.clear();
  channel_.clear();
}

void BackendPreview::setChannel(const QString& channel) {
  if (channel_ == channel)
    return;
  channel_ = channel;
  previous_modified_ = previous_size_ = -1;
}

QString BackendPreview::fileName(const QString& channel) {
  if (channel == "program")
    return "preview-final.jpg";
  if (channel == "stitched")
    return "preview-stitched.jpg";
  if (channel == "source0")
    return "preview-camera-1.jpg";
  if (channel == "source1")
    return "preview-camera-2.jpg";
  return {};
}

QImage BackendPreview::readImage(const QString& path) {
  const QFileInfo info(path);
  if (info.isSymLink() || !info.isFile() || info.size() <= 0 || info.size() > 8 * 1024 * 1024)
    throw std::runtime_error("Backend preview must be a bounded regular image file");
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    throw std::runtime_error("Could not open backend preview image");
  QImageReader reader(&file, "jpeg");
  const QSize size = reader.size();
  if (size.width() <= 0 || size.height() <= 0 || size.width() > 4096 || size.height() > 4096 ||
      static_cast<qint64>(size.width()) * size.height() > 4194304)
    throw std::runtime_error("Backend preview dimensions exceed the protocol limit");
  QImage image = reader.read();
  if (image.isNull())
    throw std::runtime_error(("Could not decode backend preview: " + reader.errorString()).toStdString());
  return image;
}

void BackendPreview::poll() {
  const QString name = fileName(channel_);
  if (directory_.isEmpty() || name.isEmpty())
    return;
  const QString path = QDir(directory_).filePath(name);
  const QFileInfo info(path);
  if (!info.exists())
    return; // The producer has not published its first frame yet.
  const qint64 modified = info.lastModified().toMSecsSinceEpoch();
  if (modified == previous_modified_ && info.size() == previous_size_)
    return;
  previous_modified_ = modified;
  previous_size_ = info.size();
  try {
    const QImage image = readImage(path);
    if (frameReady)
      frameReady(channel_, image);
  } catch (const std::exception& error) {
    if (failed)
      failed(QString::fromUtf8(error.what()));
  }
}

} // namespace hm::ui
