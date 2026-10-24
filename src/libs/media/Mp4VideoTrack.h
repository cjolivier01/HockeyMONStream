#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

#include "absl/status/statusor.h"

namespace hm::media {
enum class VideoCodec { kH264, kHevc };
struct Mp4VideoTrack {
  VideoCodec codec;
  int width, height;
  int fps_n, fps_d;
  uint64_t frames;
  int64_t start_ns, duration_ns;
};
// Read one ordinary local MP4's video-track metadata without opening a codec,
// decoding images, or probing via GstDiscoverer. The video duration excludes
// audio tails. A fragmented/ambiguous/unknown-duration recording is rejected.
// The caller provides an operation deadline and optional cancellation callback;
// no resource/time defaults are selected by this library.
absl::StatusOr<Mp4VideoTrack> ReadMp4VideoTrack(
    const std::filesystem::path& file,
    int64_t timeout_ns,
    const std::function<bool()>& is_cancelled);
} // namespace hm::media
