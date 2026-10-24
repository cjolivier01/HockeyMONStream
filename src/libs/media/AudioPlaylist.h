#pragma once

#include "hstream/src/libs/media/Mp4AudioReader.h"

namespace hm::media {
// AAC camera chapters on the same video-only clock used by VaVideoPlaylist.
// Audio outside each chapter's video extent is trimmed. Missing intervals
// inside an existing audio track become silence; a missing track is an error.
// Every returned block is bounded by maximum_frame_samples. Only Cancel may
// run concurrently. Successful Seek resets the timeline, including after EOS.
class AudioPlaylist {
 public:
  static absl::StatusOr<std::unique_ptr<AudioPlaylist>> Create(
      const Mp4AudioReaderConfig& config,
      const std::vector<std::filesystem::path>& files);
  ~AudioPlaylist();
  absl::Status Seek(int64_t time_ns);
  // End is an exclusive watermark on the global video clock. Null means that
  // watermark (or playlist EOS) was reached; a later watermark may resume it.
  // Each time boundary is rounded once to the nearest native audio sample.
  absl::StatusOr<std::optional<AudioFrame>> ReadUntil(int64_t end_ns);
  void Cancel() noexcept;
  AudioFormat format() const;
  int64_t duration_ns() const;

 private:
  struct Impl;
  explicit AudioPlaylist(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
} // namespace hm::media
