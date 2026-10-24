#pragma once

#include <array>

#include "hstream/src/libs/gpu/vaapi/VaVideoPlaylist.h"

namespace hm::gpu {
struct VaStereoVideoConfig {
  std::array<VaVideoReaderConfig, 2> readers;
  std::array<std::vector<std::filesystem::path>, 2> chapters;
  std::array<double, 2> frame_offsets;
};
struct VaStereoFrame {
  std::array<VaVideoFrame, 2> cameras;
  uint64_t sequence;
  std::array<uint64_t, 2> source_indices;
  // Synchronized source time, independent of chapter edits and seek generation.
  // The first frame can follow the requested time by less than one frame.
  GstClockTime time, duration;
};

// Owns both camera playlists on one serialized playback thread. Both cameras
// advance by one validated frame per output; neither a partial pair nor a stale
// half of a failed seek can escape. Ends at the shorter synchronized recording.
// All limits, formats, devices and offsets are supplied by the application.
// Only Cancel may run concurrently; cancellation/failure requires reconstruction.
class VaStereoVideo {
 public:
  static absl::StatusOr<std::unique_ptr<VaStereoVideo>> Create(
      const VaStereoVideoConfig& config,
      const std::function<bool()>& is_cancelled);
  ~VaStereoVideo();
  VaStereoVideo(const VaStereoVideo&) = delete;
  VaStereoVideo& operator=(const VaStereoVideo&) = delete;

  // Seek must precede Read. A successful seek restarts sequence at zero.
  // Seeking exactly duration selects EOS; backward seeks after EOS are valid.
  absl::Status Seek(GstClockTime time);
  absl::StatusOr<std::optional<VaStereoFrame>> Read();
  void Cancel() noexcept;
  GstClockTime duration() const;
  const media::Mp4VideoTrack& video_info(size_t camera) const;

 private:
  struct Impl;
  explicit VaStereoVideo(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
} // namespace hm::gpu
