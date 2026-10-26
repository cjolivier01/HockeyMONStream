#pragma once

#include <array>
#include <optional>

#include "hstream/src/libs/common/ConfigLayers.h"
#include "hstream/src/libs/common/PacedPlayback.h"
#include "hstream/src/libs/media/Mp4AudioReader.h"

namespace hm::program {
struct ProgramAudioConfig {
  size_t camera;
  media::Mp4AudioReaderConfig reader;
  size_t queue_samples;
  int bitrate;
  std::string alsa_device;
  hm::gst::PacedPlaybackSettings paced_playback;
};
// Resolve native numbered/generic audio blocks by layer. The Program playlist
// uses one source-linked audio track from a zero-offset synchronization camera.
absl::StatusOr<std::optional<ProgramAudioConfig>> ResolveProgramAudioConfig(
    const config::ProgramConfiguration& config,
    const std::array<double, 2>& frame_offsets);
} // namespace hm::program
