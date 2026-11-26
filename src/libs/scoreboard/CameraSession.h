#pragma once

#include <functional>
#include <string>
#include <vector>

#include "hstream/src/libs/scoreboard/CameraFrames.h"
#include "hstream/src/libs/scoreboard/CameraImages.h"

namespace hm::scoreboard {
struct CameraChapter {
  std::filesystem::path path;
  // Video-track timeline, excluding audio tails. Chapters must be contiguous.
  int64_t start_ns, duration_ns, stream_origin_ns;
};
struct CameraSessionConfig {
  CameraDecoderConfig decoder;
  std::vector<CameraChapter> chapters;
  // game_time = camera_time + offset. Explicit synchronization, never arrival time.
  int64_t offset_ns, game_start_ns;
  uint64_t epoch;
  std::string live_uri;
  GstClock* live_clock;
  GstClockTime live_base_time;
  unsigned live_latency_ms;
  bool live_ntp_sync;
};
// Independent bounded decode-ahead worker. One session belongs to one playback
// epoch; seeking creates a new session after cancelling the old one. This keeps
// late decoder work from crossing a seek. Video is never read back to the CPU.
class CameraSession {
 public:
  static absl::StatusOr<std::unique_ptr<CameraSession>> Start(const CameraSessionConfig& config);
  ~CameraSession();
  // Serialized final-compositor join. The previous result must be released
  // after its GPU work completes before requesting the next output frame.
  absl::StatusOr<std::shared_ptr<const gpu::Image>> Select(
      int64_t game_time_ns,
      const std::function<bool()>& cancelled = {});
  // Live startup barrier: establish a decoded interval before starting game capture.
  absl::Status WaitReady();
  void Cancel();

 private:
  struct Impl;
  explicit CameraSession(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
} // namespace hm::scoreboard
