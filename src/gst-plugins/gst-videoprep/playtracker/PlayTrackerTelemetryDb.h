#pragma once
#include <functional>
#include <memory>
#include "hstream/src/gst-plugins/gst-videoprep/playtracker/PlayTrackerTelemetryCsv.h"

namespace hm::playtracker {
// Immutable, shared CPU metadata. Encoding is invoked once by the writer thread.
struct TelemetryGeometry {
  uint32_t width{0}, height{0};
  std::string revision;
  std::function<std::string()> encode_mask;
};
class PlayTrackerTelemetryDb {
 public:
  PlayTrackerTelemetryDb();
  ~PlayTrackerTelemetryDb();
  absl::Status Start(
      const std::string& directory,
      TelemetryConfigArtifact source,
      TelemetryConfigArtifact effective,
      std::vector<TelemetryConfigEvent> startup = {},
      size_t capacity = 2048,
      const std::string& game_id = {});
  void Stop();
  void MarkRunOutcome(TelemetryRunOutcome outcome);
  bool active() const;
  bool failed() const;
  bool TryEnqueue(TelemetrySample sample, std::shared_ptr<const TelemetryGeometry> geometry = {});
  bool TryRecordConfigEvent(TelemetryConfigEvent event);
  bool TryRecordDiscontinuity(TelemetryConfigEvent event);
  std::string output_manifest() const; // Retains the pipeline notification API; returns the database path.
  uint64_t frame_id_high_watermark() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace hm::playtracker
