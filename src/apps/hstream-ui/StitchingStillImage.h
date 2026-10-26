#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtGui/QImage>

#include <functional>
#include <mutex>

struct SavedStitchingStill {
  QImage image;
  QString name;
  QByteArray revision;
  bool retry{false};
};

QByteArray StitchingStillRevision(const QString& directory);

// Synchronous, bounded, advisory read shared by Qt and the web adapter. The
// caller's metadata gate is drained by Qt cancellation; no artifact/config
// lock survives into image decoding. No recovery or generation publication is
// performed. A changed final revision retires the image and requests a retry.
SavedStitchingStill ReadSavedStitchingStill(
    const QString& directory,
    const QString& expected_generation,
    const QByteArray& expected_settings,
    const QByteArray& expected_revision,
    std::mutex& metadata_mutex,
    const std::function<bool()>& cancelled = {},
    const std::function<void()>& metadata_locked = {},
    const std::function<void()>& decoding = {});
