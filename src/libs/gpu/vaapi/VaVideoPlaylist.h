#pragma once

#include <functional>
#include <vector>

#include "hstream/src/libs/gpu/vaapi/VaVideoReader.h"
#include "hstream/src/libs/media/Mp4VideoTrack.h"

namespace hm::gpu {
// A single camera's ordered local MP4 chapters. Positions are on a continuous
// video clock; audio/container tails never advance that clock. One serialized
// owner except Cancel(). Metadata probing never opens a software codec.
class VaVideoPlaylist {
 public:
  static absl::StatusOr<std::unique_ptr<VaVideoPlaylist>> Create(
      const VaVideoReaderConfig& config,
      const std::vector<std::filesystem::path>& files,
      const std::function<bool()>& is_cancelled);
  ~VaVideoPlaylist();
  VaVideoPlaylist(const VaVideoPlaylist&) = delete;
  VaVideoPlaylist& operator=(const VaVideoPlaylist&) = delete;

  // Seek must precede Read. Exactly duration() selects clean EOS. Subsequent
  // seeks remain supported; the decoder flushes or changes chapters as needed.
  absl::Status Seek(GstClockTime time);
  // Frame-index seeks map through chapter frame counts and use the selected
  // chapter's own frame grid. This avoids nanosecond duration rounding at
  // chapter boundaries. Requires constant-rate metadata in every chapter;
  // Read then validates the actual frame grid and counts, retiring on mismatch.
  // At most one clipped 1ns preceding picture from B-frame demux rounding is
  // discarded after a seek, without reading any pixels back to the host.
  absl::Status SeekFrame(uint64_t frame);
  // Returned frame.time is global video time. The sample's segment/PTS remain
  // file-relative and unmodified. Returned samples outlive transitions/owner.
  absl::StatusOr<std::optional<VaVideoFrame>> Read();
  void Cancel() noexcept;
  GstClockTime duration() const;
  uint64_t frame_count() const;
  const media::Mp4VideoTrack& video_info() const;

 private:
  struct Impl;
  explicit VaVideoPlaylist(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
} // namespace hm::gpu
