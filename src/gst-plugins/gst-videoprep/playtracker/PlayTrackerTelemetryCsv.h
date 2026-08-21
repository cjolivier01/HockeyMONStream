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
  void Stop();

  bool active() const;
  bool TryEnqueue(TelemetrySample sample);
  bool TryRecordConfigEvent(TelemetryConfigEvent event);

  uint64_t next_sample_id() const {
    return next_sample_id_.load(std::memory_order_acquire) + 1;
  }
  uint64_t dropped_samples() const {
    return dropped_samples_.load(std::memory_order_acquire);
  }
  std::string output_manifest() const;

 private:
  struct QueuedSample {
    uint64_t sample_id{0};
    TelemetrySample sample;
  };
  struct QueuedConfigEvent {
    uint64_t sample_boundary{0};
    TelemetryConfigEvent event;
  };
  using WorkItem = std::variant<QueuedSample, QueuedConfigEvent>;

  absl::Status OpenOutputs(
      const std::string& output_directory,
      const std::string& source_config_file,
      const std::string& effective_config_file);
  void WriterLoop();
  void WriteSample(const QueuedSample& queued);
  void WriteConfigEvent(const QueuedConfigEvent& queued);
  void WriteManifest(bool complete);
  void CloseOutputs();

  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<WorkItem> queue_;
  size_t queue_capacity_{0};
  bool active_{false};
  bool stopping_{false};
  bool writer_failed_{false};
  std::thread writer_thread_;
  std::atomic<uint64_t> next_sample_id_{0};
  std::atomic<uint64_t> dropped_samples_{0};
  std::atomic<uint64_t> dropped_config_events_{0};
  uint64_t config_event_sequence_{0};
  uint64_t config_artifact_sequence_{0};

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
