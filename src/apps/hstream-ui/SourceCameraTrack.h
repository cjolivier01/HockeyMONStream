#pragma once

#include <QtCore/QString>
#include <functional>
#include "hstream/src/libs/media/Mp4VideoTrack.h"

namespace hm::ui {
// Header/packet metadata only. start_ns is the physical video presentation
// origin; individual playback demuxers may normalize their stream-time clock.
struct SourceCameraTrack {
  media::VideoCodec codec;
  int width, height;
  int64_t start_ns, duration_ns;
  const char* demux;
};
absl::StatusOr<SourceCameraTrack> ReadSourceCameraTrack(
    const QString& file,
    qint64 timeout_ms,
    const std::function<bool()>& cancelled = {});
} // namespace hm::ui
