#include "hstream/src/libs/common/PacedPlaybackConfig.h"

#include <limits>
#include <stdexcept>

namespace hm::gst {
absl::StatusOr<PacedPlaybackSettings> ReadPacedPlaybackSettings(const YAML::Node& config) {
  try {
    const auto local = config["runtime"]["local_playback"];
    if (!local.IsMap())
      throw std::invalid_argument("runtime.local_playback must declare paced presentation bounds");
    const auto integer = [&](const char* name, guint64 minimum, guint64 maximum) {
      if (!local[name].IsScalar())
        throw std::invalid_argument(std::string("Missing local playback bound: ") + name);
      const auto value = local[name].as<gint64>();
      if (value < 0 || guint64(value) < minimum || guint64(value) > maximum)
        throw std::invalid_argument(std::string("Invalid local playback bound: ") + name);
      return guint64(value);
    };
    PacedPlaybackSettings settings{
        integer("audio_buffer_us", 1, INT64_MAX / GST_USECOND),
        integer("audio_period_us", 1, INT64_MAX / GST_USECOND),
        integer("processing_deadline_ms", 0, INT64_MAX / GST_MSECOND) * GST_MSECOND,
        guint(integer("history_frames", 2, G_MAXUINT)),
        integer("mapping_timeout_ms", 1, INT64_MAX / GST_MSECOND) * GST_MSECOND,
        guint(integer("audio_queue_buffers", 1, G_MAXUINT)),
        guint(integer("audio_queue_bytes", 1, G_MAXUINT)),
        guint(integer("preview_frames", 2, G_MAXUINT)),
        integer("pause_timeout_ms", 1, INT64_MAX / GST_MSECOND) * GST_MSECOND};
    if (settings.device_period_us > settings.device_buffer_us)
      throw std::invalid_argument("Local audio period exceeds its device buffer");
    return settings;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  }
}
} // namespace hm::gst
