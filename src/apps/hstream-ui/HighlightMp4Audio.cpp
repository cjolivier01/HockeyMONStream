#include "src/apps/hstream-ui/HighlightMp4Audio.h"

#include <QtCore/QFile>
#include <limits>

#include "hstream/src/libs/media/Mp4AudioEdit.h"

namespace hm::ui {
bool FinalizeHighlightMp4Audio(const QString& path, qint64 duration_ms, QString* error) {
  if (duration_ms <= 0 || duration_ms > std::numeric_limits<qint64>::max() / 48) {
    if (error)
      *error = "Invalid reel audio duration";
    return false;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadWrite)) {
    if (error)
      *error = file.errorString();
    return false;
  }
  const auto status = media::FinalizeMp4Audio(file.handle(), 48000, duration_ms * 48);
  if (!status.ok() && error)
    *error = QString::fromStdString(status.ToString());
  return status.ok();
}
} // namespace hm::ui
