#pragma once

#include <optional>

#include "hstream/src/libs/common/PacedPlayback.h"
#include "hstream/src/libs/gpu/vaapi/VaOutputPool.h"
#include "hstream/src/libs/media/Mp4AudioReader.h"

namespace hm::program {
enum class VaProgramSinkKind { kDiscard, kHevcMp4, kPreview };
struct VaProgramAudioConfig {
  media::AudioFormat format;
  size_t queue_samples;
  int bitrate;
  std::string alsa_device;
  bool muted;
  std::optional<hm::gst::PacedPlaybackSettings> paced_playback;
};
struct VaProgramSinkConfig {
  VaProgramSinkKind kind;
  int device;
  GstVideoInfo video;
  unsigned pool_frames; // Preview needs at least two: displayed frame and replacement.
  GstClockTime operation_timeout, cancellation_poll;
  bool realtime;
  // Encode mode duplicates this empty regular file's writable descriptor at
  // offset zero. No other writer may use it during this owner. Its caller owns
  // output naming, exclusive creation, publication and cleanup after failure.
  // With audio it must be O_RDWR, for the post-EOS AAC metadata edit.
  int output_fd;
  unsigned bitrate_kbps, keyframe_interval;
  size_t maximum_encoded_packet_bytes;
  // Preview mode accepts an external X11 window or zero for a sink-owned one.
  uintptr_t window;
  // Optional caller-owned GL display context, retained by the pipeline. Any
  // externally wrapped EGL/GBM/native-display resources must outlive this sink.
  GstContext* gl_context;
  std::optional<VaProgramAudioConfig> audio;
};

// Bounded GPU output owner. Acquire returns an exclusively owned linear VA
// surface for AmdProgramProcessor::Render. Push consumes that surface, including
// on failure. No video map/readback occurs here. One thread owns all operations
// and destruction except concurrent Cancel; driver calls are not interruptible.
class VaProgramSink {
 public:
  static absl::StatusOr<std::unique_ptr<VaProgramSink>> Create(const VaProgramSinkConfig& config);
  ~VaProgramSink();
  VaProgramSink(const VaProgramSink&) = delete;
  VaProgramSink& operator=(const VaProgramSink&) = delete;

  absl::StatusOr<GstBuffer*> Acquire();
  absl::Status Push(GstBuffer* buffer, GstClockTime time, GstClockTime duration);
  // Interleaved float audio on a zero-based sample clock. Frames must be
  // contiguous; the caller trims/fills the recording timeline before pushing.
  absl::Status PushAudio(const media::AudioFrame& frame);
  // Drain output through mux/sink EOS with a bounded, cancellable wait. File
  // publication is permitted only after this succeeds.
  absl::Status Finish();
  absl::Status SetWindow(uintptr_t window);
  // Changes visibility at a complete frame boundary. Disabling fences earlier
  // GL draws; subsequent exposes draw black. Enabling starts a new readiness
  // epoch, admitting only frames at/after minimum_time on this output timeline.
  absl::Status SetPreviewVisible(bool visible, GstClockTime minimum_time);
  // True only after an admitted GL draw and a GL-thread completion barrier.
  // Buffer submission/base-sink statistics do not imply window presentation.
  absl::StatusOr<bool> PreviewReady();
  // Call between submissions. PAUSED freezes the shared audio/video clock;
  // preroll may remain pending until resume supplies the next input. No waits
  // or application operation deadlines remain active after this returns.
  absl::Status SetPaused(bool paused);
  absl::Status SetAudioMuted(bool muted);
  // Local preview/discard only. Flush queued audio/video through READY and
  // restart the existing graph with timestamp/sample origin zero. The VA pool,
  // native window and mute choice survive; encoded archives cannot seek.
  absl::Status ResetTimeline();
  void Cancel() noexcept;
  const GstVideoInfo& video_info() const;

 private:
  struct Impl;
  explicit VaProgramSink(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
} // namespace hm::program
