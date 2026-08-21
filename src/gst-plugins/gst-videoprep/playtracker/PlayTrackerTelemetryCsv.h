#pragma once

#include "absl/status/status.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace hm::playtracker {

class PlayTrackerTelemetryCsvTestPeer;

struct TelemetryTrack {
  uint64_t tracking_id{0};
  float left{0.0f};
  float top{0.0f};
  float width{0.0f};
  float height{0.0f};
  float score{0.0f};
  int class_id{0};
};

struct TelemetryBox {
  float left{0.0f};
  float top{0.0f};
  float width{0.0f};
  float height{0.0f};
};

struct TelemetrySample {
  uint64_t source_frame{0};
  uint32_t source_id{0};
  std::optional<uint32_t> decoded_source_id;
  std::optional<uint64_t> decoded_sequence;
  std::optional<uint64_t> pts_ns;
  std::optional<uint64_t> ntp_ns;
  uint64_t seek_epoch{0};
  uint32_t width{0};
  uint32_t height{0};
  std::vector<TelemetryTrack> tracks;
  // Native playtracker order: first is the fast policy box, last is the
  // follower/Program camera box. A one-box policy uses the same box for both.
  std::vector<TelemetryBox> policy_boxes;
};

struct TelemetryConfigEvent {
  std::string kind;
  std::string key;
  std::string value;
  std::string artifact_stem;
  std::string artifact_contents;
};

struct TelemetryConfigArtifact {
  std::string path;
  std::string contents;
};

enum class TelemetryRunOutcome {
  kIncomplete,
  kEndOfStream,
  kIntentionalStop,
  kFailed,
};

/**
 * Bounded, non-blocking metadata exporter for HM camera-policy datasets.
 *
 * TryEnqueue() copies only small CPU-resident DeepStream/playtracker metadata.
 * It never owns, maps, or reads a video surface. Disk I/O runs on writer_thread_.
 */
class PlayTrackerTelemetryCsv {
 public:
  PlayTrackerTelemetryCsv() = default;
  ~PlayTrackerTelemetryCsv();

  PlayTrackerTelemetryCsv(const PlayTrackerTelemetryCsv&) = delete;
  PlayTrackerTelemetryCsv& operator=(const PlayTrackerTelemetryCsv&) = delete;

  absl::Status Start(
      const std::string& output_directory,
      const std::string& source_config_file,
      const std::string& effective_config_file,
      size_t queue_capacity = 2048);
  absl::Status Start(
      const std::string& output_directory,
      TelemetryConfigArtifact source_config,
      TelemetryConfigArtifact effective_config,
      std::vector<TelemetryConfigEvent> startup_config_events = {},
      size_t queue_capacity = 2048);
  void MarkRunOutcome(TelemetryRunOutcome outcome);
  void Stop();

  bool active() const;
  bool TryEnqueue(TelemetrySample sample);
  bool TryRecordConfigEvent(TelemetryConfigEvent event);
  bool TryRecordDiscontinuity(TelemetryConfigEvent event);

  uint64_t frame_id_high_watermark() const {
    return frame_id_high_watermark_.load(std::memory_order_acquire);
  }
  uint64_t attempted_samples() const {
    return attempted_samples_.load(std::memory_order_acquire);
  }
  uint64_t dropped_samples() const {
    return dropped_samples_.load(std::memory_order_acquire);
  }
  std::string output_manifest() const;

 private:
  friend class PlayTrackerTelemetryCsvTestPeer;

  struct QueuedSample {
    uint64_t sample_id{0};
    TelemetrySample sample;
  };
  struct QueuedConfigEvent {
    uint64_t sample_boundary{0};
    TelemetryConfigEvent event;
  };
  struct OwnedArtifact {
    std::string filename;
    // Training inputs are written to filename while the run is active, then
    // atomically linked to this HM-visible name after successful completion.
    // The retained descriptor also makes every staged artifact independently
    // fsyncable without reopening a mutable pathname.
    std::string published_filename;
    uint64_t device{0};
    uint64_t inode{0};
    int reservation_fd{-1};
    bool training_input{false};
  };
  using WorkItem = std::variant<QueuedSample, QueuedConfigEvent>;

  absl::Status OpenOutputs(
      const std::string& output_directory,
      const TelemetryConfigArtifact& source_config,
      const TelemetryConfigArtifact& effective_config);
  bool TryEnqueueLocked(TelemetrySample sample);
  bool TryRecordConfigEventLocked(TelemetryConfigEvent event);
  void WriterLoop();
  void WriteSample(const QueuedSample& queued);
  bool WriteConfigEvent(const QueuedConfigEvent& queued);
  std::string BuildManifestContents() const;
  bool WriteManifestAndSync(const std::string& phase);
  bool WriteExclusiveConfigArtifact(const std::string& stem, const std::string& contents, std::string* filename);
  bool FlushAndSyncStagedArtifacts();
  bool SyncFd(int fd, const std::string& event);
  bool SyncDirectory(const std::string& phase);
  bool PublishTrainingArtifacts();
  bool VerifyOwnedArtifacts() const;
  void RemoveOwnedArtifacts(bool training_only);
  void CloseOutputs();
  void RemoveIncompleteOutputs();
  void ResetOutputPaths();

  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<WorkItem> queue_;
  size_t queue_capacity_{0};
  bool active_{false};
  bool stopping_{false};
  bool writer_failed_{false};
  bool writer_drained_{false};
  bool completed_{false};
  bool eligible_for_training_{false};
  bool publication_committed_{false};
  TelemetryRunOutcome run_outcome_{TelemetryRunOutcome::kIncomplete};
  std::thread writer_thread_;
  std::atomic<uint64_t> frame_id_high_watermark_{0};
  std::atomic<uint64_t> attempted_samples_{0};
  std::atomic<uint64_t> discontinuity_gaps_{0};
  std::atomic<uint64_t> config_event_discontinuity_gaps_{0};
  std::atomic<uint64_t> dropped_samples_{0};
  std::atomic<uint64_t> dropped_config_events_{0};
  std::atomic<uint64_t> samples_buffered_{0};
  std::atomic<uint64_t> training_samples_buffered_{0};
  std::atomic<uint64_t> config_events_buffered_{0};
  std::atomic<uint64_t> samples_persisted_{0};
  std::atomic<uint64_t> training_samples_persisted_{0};
  std::atomic<uint64_t> config_events_attempted_{0};
  std::atomic<uint64_t> config_events_persisted_{0};
  std::atomic<bool> fail_next_config_artifact_write_for_testing_{false};
  std::string fail_sync_event_for_testing_;
  std::string fail_directory_syncs_from_event_for_testing_;
  bool fail_all_directory_syncs_for_testing_{false};
  std::vector<std::string> durability_events_for_testing_;
  uint64_t config_event_sequence_{0};
  uint64_t config_artifact_sequence_{0};
  uint64_t manifest_rewrite_sequence_{0};

  int output_directory_fd_{-1};
  int directory_lock_fd_{-1};
  int manifest_fd_{-1};
  std::vector<OwnedArtifact> owned_artifacts_;

  std::string started_utc_;
  std::string output_directory_;
  std::string suffix_;
  std::string manifest_path_;
  std::string tracking_filename_;
  std::string camera_filename_;
  std::string camera_fast_filename_;
  std::string frame_index_filename_;
  std::string config_events_filename_;
  std::string source_config_filename_;
  std::string effective_config_filename_;
  std::string source_config_path_;
  std::string effective_config_path_;

  std::ofstream tracking_;
  std::ofstream camera_;
  std::ofstream camera_fast_;
  std::ofstream frame_index_;
  std::ofstream config_events_;
};

std::string ReadTelemetryConfigArtifact(const std::string& path);

} // namespace hm::playtracker
