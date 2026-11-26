#pragma once

#include <gst/gst.h>
#include <gst/video/video-info.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>

#include "absl/status/statusor.h"
#include "hstream/src/libs/scoreboard/CameraRegion.h"

namespace hm::scoreboard {
enum class CameraDecodeBackend { kNvidia, kVa };
enum class CameraCodec { kH264, kHevc };
struct CameraDecoderConfig {
  CameraDecodeBackend backend;
  CameraCodec codec;
  bool live;
  unsigned gpu_id;
  std::filesystem::path render_node;
  int source_width, source_height;
  CameraRegion region;
  unsigned queued_frames, extra_surfaces;
  GstClockTime operation_timeout, cancellation_poll_interval;
};
struct CameraDecodedFrame {
  std::unique_ptr<GstSample, decltype(&gst_sample_unref)> sample{nullptr, gst_sample_unref};
  GstVideoInfo info{};
  // Stream time and duration, never wall-clock arrival time. The coordinator
  // maps chapter/source timing to the game clock before the compositor join.
  // Live duration may be unknown; the session waits for the next actual PTS.
  GstClockTime time = GST_CLOCK_TIME_NONE, duration = GST_CLOCK_TIME_NONE;
  // NVIDIA crops during GPU conversion; VA leaves a linear NV12 surface whose
  // ROI can be sampled directly through CropCameraImage without a host copy.
  bool cropped = false;
};
// Independent video-only hardware decode, with bounded GPU output buffering.
// No frame dropping, software fallback, audio decode or stitch-mux admission.
// One serialized owner except Cancel(); returned samples own their surfaces.
class CameraDecoder {
 public:
  static absl::StatusOr<std::unique_ptr<CameraDecoder>> Create(const CameraDecoderConfig& config);
  ~CameraDecoder();
  absl::Status OpenFile(const std::filesystem::path& path, GstClockTime start);
  // The live game and scoreboard graphs share this clock/base time. RTP video
  // is selected explicitly; audio does not enter this graph. No leaky queue.
  absl::Status OpenLive(
      const std::string& uri,
      GstClock* clock,
      GstClockTime base_time,
      unsigned latency_ms,
      bool ntp_sync);
  absl::Status Seek(GstClockTime time);
  absl::StatusOr<std::optional<CameraDecodedFrame>> Read();
  void Cancel() noexcept;

 private:
  struct Impl;
  explicit CameraDecoder(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
} // namespace hm::scoreboard
