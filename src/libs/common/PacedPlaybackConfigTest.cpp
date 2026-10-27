#include "hstream/src/libs/common/PacedPlaybackConfig.h"

#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* detail) {
  if (!condition)
    throw std::runtime_error(detail);
}
} // namespace

int main(int argc, char** argv) {
  try {
    Check(argc == 2, "Expected baseline YAML");
    const auto baseline = YAML::LoadFile(argv[1]);
    const auto resolved = hm::gst::ReadPacedPlaybackSettings(baseline);
    Check(resolved.ok(), "Canonical local playback bounds did not resolve");
    for (const auto* name :
         {"audio_buffer_us",
          "audio_period_us",
          "processing_deadline_ms",
          "history_frames",
          "mapping_timeout_ms",
          "audio_queue_buffers",
          "audio_queue_bytes",
          "preview_frames",
          "pause_timeout_ms"}) {
      auto incomplete = YAML::Clone(baseline);
      incomplete["runtime"]["local_playback"].remove(name);
      Check(!hm::gst::ReadPacedPlaybackSettings(incomplete).ok(), "Missing bound received a fallback");
      auto invalid = YAML::Clone(baseline);
      invalid["runtime"]["local_playback"][name] = -1;
      Check(!hm::gst::ReadPacedPlaybackSettings(invalid).ok(), "Negative bound accepted");
    }
    for (const auto* name : {"history_frames", "preview_frames"}) {
      auto invalid = YAML::Clone(baseline);
      invalid["runtime"]["local_playback"][name] = 1;
      Check(!hm::gst::ReadPacedPlaybackSettings(invalid).ok(), "Insufficient frame retention accepted");
    }
    auto invalid = YAML::Clone(baseline);
    invalid["runtime"]["local_playback"]["audio_period_us"] = resolved->device_buffer_us + 1;
    Check(!hm::gst::ReadPacedPlaybackSettings(invalid).ok(), "Audio period larger than buffer accepted");
    invalid = YAML::Clone(baseline);
    invalid["runtime"]["local_playback"]["mapping_timeout_ms"] = "9223372036854775807";
    Check(!hm::gst::ReadPacedPlaybackSettings(invalid).ok(), "Nanosecond conversion overflow accepted");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
