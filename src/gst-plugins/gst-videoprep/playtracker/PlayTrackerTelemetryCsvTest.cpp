#include "hstream/src/gst-plugins/gst-videoprep/playtracker/PlayTrackerTelemetryCsv.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace hm::playtracker {

class PlayTrackerTelemetryCsvTestPeer {
 public:
  static bool WaitUntilQueueEmpty(PlayTrackerTelemetryCsv& exporter) {
    for (size_t attempt = 0; attempt < 1'000'000; ++attempt) {
      {
        std::lock_guard<std::mutex> lock(exporter.mutex_);
        if (exporter.queue_.empty()) {
          return true;
        }
      }
      std::this_thread::yield();
    }
    return false;
  }

  static void FailNextConfigArtifactWrite(PlayTrackerTelemetryCsv& exporter) {
    exporter.fail_next_config_artifact_write_for_testing_.store(true, std::memory_order_release);
  }

  static void FailFirstStagedSync(PlayTrackerTelemetryCsv& exporter) {
    for (const PlayTrackerTelemetryCsv::OwnedArtifact& artifact : exporter.owned_artifacts_) {
      if (artifact.reservation_fd >= 0) {
        exporter.fail_sync_event_for_testing_ = "fsync:staged:" + artifact.filename;
        return;
      }
    }
  }

  static void FailSync(PlayTrackerTelemetryCsv& exporter, std::string event) {
    exporter.fail_sync_event_for_testing_ = std::move(event);
  }

  static void FailDirectorySyncsFrom(PlayTrackerTelemetryCsv& exporter, std::string event) {
    exporter.fail_directory_syncs_from_event_for_testing_ = std::move(event);
  }

  static std::vector<std::string> DurabilityEvents(const PlayTrackerTelemetryCsv& exporter) {
    return exporter.durability_events_for_testing_;
  }

  static size_t OpenArtifactDescriptorCount(const PlayTrackerTelemetryCsv& exporter) {
    return static_cast<size_t>(std::count_if(
        exporter.owned_artifacts_.begin(),
        exporter.owned_artifacts_.end(),
        [](const PlayTrackerTelemetryCsv::OwnedArtifact& artifact) { return artifact.reservation_fd >= 0; }));
  }

  static bool WaitUntilConfigEventsBuffered(PlayTrackerTelemetryCsv& exporter, uint64_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      if (exporter.config_events_buffered_.load(std::memory_order_acquire) >= expected) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  }

  static bool RewriteManifestState(PlayTrackerTelemetryCsv& exporter, bool committed, const std::string& phase) {
    exporter.publication_committed_ = committed;
    exporter.completed_ = committed;
    exporter.eligible_for_training_ = committed;
    return exporter.WriteManifestAndSync(phase);
  }
};

} // namespace hm::playtracker

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

std::string read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::vector<std::string> split_csv_without_quotes(const std::string& line) {
  std::vector<std::string> fields;
  std::istringstream input(line);
  for (std::string field; std::getline(input, field, ',');) {
    fields.push_back(field);
  }
  return fields;
}

std::vector<uint64_t> csv_frame_ids(const std::string& contents) {
  std::vector<uint64_t> ids;
  std::istringstream input(contents);
  for (std::string line; std::getline(input, line);) {
    if (line.empty()) {
      continue;
    }
    ids.push_back(std::stoull(line.substr(0, line.find(','))));
  }
  return ids;
}

size_t event_position(const std::vector<std::string>& events, const std::string& expected) {
  const auto event = std::find(events.begin(), events.end(), expected);
  return event == events.end() ? events.size() : static_cast<size_t>(std::distance(events.begin(), event));
}

hm::playtracker::TelemetrySample make_policy_sample(bool with_track, size_t extra_tracks = 0) {
  hm::playtracker::TelemetrySample sample;
  sample.width = 3840;
  sample.height = 1080;
  sample.detections.push_back({9.0f, 19.0f, 32.0f, 42.0f, 0.625f, 0});
  if (with_track) {
    sample.tracks.push_back({88, 10.5f, 20.25f, 30.0f, 40.0f, 0.875f, 0});
  }
  sample.tracks.resize(sample.tracks.size() + extra_tracks, {99, 1.0f, 2.0f, 3.0f, 4.0f, 0.5f, 0});
  sample.policy_boxes.push_back({1.0f, 2.0f, 300.0f, 150.0f});
  sample.policy_boxes.push_back({3.0f, 4.0f, 500.0f, 250.0f});
  return sample;
}

} // namespace

int main() {
  const char* bazel_test_tmpdir = std::getenv("TEST_TMPDIR");
  const fs::path temporary_root = bazel_test_tmpdir ? fs::path(bazel_test_tmpdir) : fs::temp_directory_path();
  const fs::path directory =
      temporary_root / ("hstream-playtracker-telemetry-test-" + std::to_string(static_cast<uint64_t>(::getpid())));
  std::error_code error;
  fs::remove_all(directory, error);
  fs::create_directories(directory, error);
  if (!expect(!error, "could not create isolated test directory")) {
    return 1;
  }

  const fs::path source_config = directory / "source.yaml";
  const fs::path effective_config = directory / "effective.yaml";
  std::ofstream(source_config) << "play-tracker:\n  controller: rule\n";
  std::ofstream(effective_config) << "play-tracker:\n  controller: rule\n  runtime: effective\n";
  // An HM-produced artifact must never be overwritten merely because no
  // hstream manifest exists beside it.
  std::ofstream(directory / "tracking.csv") << "preserve-existing-hm-data\n";
  std::ofstream(directory / "camera-9.csv") << "preserve-latest-generation\n";

  hm::playtracker::PlayTrackerTelemetryCsv exporter;
  const absl::Status start = exporter.Start(directory.string(), source_config.string(), effective_config.string(), 8);
  if (!expect(start.ok(), start.ToString())) {
    return 1;
  }
  if (!expect(
          exporter.output_manifest() == (directory / "hstream_telemetry-10.json").string(),
          "existing HM files should advance beyond the highest generation for every schema")) {
    return 1;
  }
  if (!expect(
          !fs::exists(directory / "tracking-10.csv") && !fs::exists(directory / "camera-10.csv") &&
              !fs::exists(directory / "camera_fast-10.csv") && !fs::exists(directory / "detections-10.csv"),
          "an active run must not expose HM-discoverable training filenames")) {
    return 1;
  }
  const std::string active_manifest = read_file(directory / "hstream_telemetry-10.json");
  if (!expect(
          active_manifest.find("\"publication_state\": \"pending\"") != std::string::npos &&
              active_manifest.find("\"completed\": false") != std::string::npos &&
              active_manifest.find("\"eligible_for_training\": false") != std::string::npos,
          "the durable active manifest must remain pending and ineligible before HM publication")) {
    return 1;
  }

  hm::playtracker::TelemetrySample sample;
  sample.source_id = 7;
  sample.source_frame = 991;
  sample.decoded_source_id = 2;
  sample.decoded_sequence = 1234;
  sample.pts_ns = 17'490'000'000ULL;
  sample.ntp_ns = 42;
  sample.seek_epoch = 3;
  sample.width = 3840;
  sample.height = 1080;
  sample.detections.push_back({8.0f, 18.0f, 34.0f, 44.0f, 0.75f, 0});
  sample.tracks.push_back({88, 10.5f, 20.25f, 30.0f, 40.0f, 0.875f, 0});
  sample.policy_boxes.push_back({1.0f, 2.0f, 300.0f, 150.0f});
  sample.policy_boxes.push_back({3.0f, 4.0f, 500.0f, 250.0f});
  if (!expect(exporter.TryEnqueue(std::move(sample)), "metadata sample should enter the bounded writer queue") ||
      !expect(
          exporter.TryRecordConfigEvent(
              {"runtime-tuning", "max-speed-x", "24", "runtime", "play-tracker:\n  max-speed-x: 24\n"}),
          "runtime config event should enter the writer queue")) {
    return 1;
  }
  if (!expect(
          exporter.TryRecordDiscontinuity({"seek", "flush-stop", "1", {}, {}}),
          "seek should reserve an explicit frame-ID gap") ||
      !expect(exporter.TryEnqueue(make_policy_sample(false)), "trackless camera sample should be retained") ||
      !expect(
          exporter.TryRecordConfigEvent({"property", "fixed-edge-rotation-angle", "31", {}, {}}),
          "geometry update should be recorded before the next sample") ||
      !expect(exporter.TryEnqueue(make_policy_sample(true)), "post-seek tracked sample should be retained")) {
    return 1;
  }
  exporter.MarkRunOutcome(hm::playtracker::TelemetryRunOutcome::kIntentionalStop);
  exporter.Stop();

  const std::string tracking = read_file(directory / "tracking-10.csv");
  const std::vector<std::string> tracking_fields = split_csv_without_quotes(tracking.substr(0, tracking.find('\n')));
  const std::string detections = read_file(directory / "detections-10.csv");
  const std::vector<std::string> detection_fields =
      split_csv_without_quotes(detections.substr(0, detections.find('\n')));
  const std::string camera = read_file(directory / "camera-10.csv");
  const std::string camera_fast = read_file(directory / "camera_fast-10.csv");
  const std::string frame_index = read_file(directory / "hstream_frame_index-10.csv");
  const std::string config_events = read_file(directory / "hstream_config_events-10.csv");
  const std::string manifest = read_file(directory / "hstream_telemetry-10.json");
  const std::vector<std::string> durability_events =
      hm::playtracker::PlayTrackerTelemetryCsvTestPeer::DurabilityEvents(exporter);
  const size_t pending_manifest_sync = event_position(durability_events, "fsync:manifest:pending");
  const size_t camera_link = event_position(durability_events, "link:camera-10.csv");
  const size_t fast_camera_link = event_position(durability_events, "link:camera_fast-10.csv");
  const size_t detections_link = event_position(durability_events, "link:detections-10.csv");
  const size_t input_directory_sync = event_position(durability_events, "fsync:directory:inputs");
  const size_t tracking_link = event_position(durability_events, "link:tracking-10.csv");
  const size_t tracking_commit_sync = event_position(durability_events, "fsync:directory:tracking-commit");
  const size_t committed_manifest_sync = event_position(durability_events, "fsync:manifest:committed");

  const bool valid =
      expect(
          read_file(directory / "tracking.csv") == "preserve-existing-hm-data\n",
          "pre-existing HM tracking data must remain byte-for-byte intact") &&
      expect(tracking_fields.size() == 13, "tracking CSV must use HM's current 13-column headerless schema") &&
      expect(
          detection_fields.size() == 7 && detection_fields[0] == "1" && detection_fields[1] == "8" &&
              detection_fields[2] == "18" && detection_fields[3] == "42" && detection_fields[4] == "62" &&
              detection_fields[5] == "0.75" && detection_fields[6] == "0",
          "detections CSV must use HM's current seven-column headerless TLBR schema") &&
      expect(
          tracking_fields[0] == "1" && tracking_fields[1] == "88" && tracking_fields[8] == "-1" &&
              tracking_fields[9] == "{}" && tracking_fields[10].empty() && tracking_fields[11] == "0" &&
              tracking_fields[12] == "-1",
          "tracking CSV identity and optional compatibility columns should match HM") &&
      expect(camera.rfind("1,3,4,500,250", 0) == 0, "camera.csv should contain the follower/Program policy action") &&
      expect(camera_fast.rfind("1,1,2,300,150", 0) == 0, "camera_fast.csv should contain the fast policy action") &&
      expect(
          csv_frame_ids(tracking) == std::vector<uint64_t>({1, 6}),
          "seek and trackless intervals must remain visible as numeric gaps in tracking.csv") &&
      expect(
          csv_frame_ids(camera) == std::vector<uint64_t>({1, 4, 6}),
          "trackless samples should keep their camera action without collapsing the timeline") &&
      expect(
          frame_index.find("1,7,991,2,1234,17490000000,42,3,3840,1080,1,1,1") != std::string::npos,
          "frame sidecar should preserve source, native frame, timestamps, dimensions, and detection/track counts") &&
      expect(
          config_events.find("runtime-tuning,max-speed-x,24,runtime-10-1.yaml") != std::string::npos &&
              read_file(directory / "runtime-10-1.yaml").find("max-speed-x: 24") != std::string::npos,
          "runtime policy changes should retain exact configuration artifacts") &&
      expect(
          config_events.find("seek,flush-stop,1,") != std::string::npos &&
              config_events.find(",4,seek,flush-stop,1,") != std::string::npos,
          "seek config event should identify the first post-gap sample boundary") &&
      expect(
          config_events.find(",6,property,fixed-edge-rotation-angle,31,") != std::string::npos,
          "policy config event should be ordered at the next sample boundary") &&
      expect(
          manifest.find("\"completed\": true") != std::string::npos &&
              manifest.find("\"publication_state\": \"committed\"") != std::string::npos &&
              manifest.find("\"eligible_for_training\": true") != std::string::npos &&
              manifest.find("\"frame_id_high_watermark\": 6") != std::string::npos &&
              manifest.find("\"samples_attempted\": 3") != std::string::npos &&
              manifest.find("\"discontinuity_gaps\": 3") != std::string::npos &&
              manifest.find("\"config_events_attempted\": 3") != std::string::npos &&
              manifest.find("\"config_events_persisted\": 3") != std::string::npos &&
              manifest.find("\"config_events_lost\": 0") != std::string::npos &&
              manifest.find("\"dropped_samples\": 0") != std::string::npos &&
              manifest.find("\"lossless_queue\": true") != std::string::npos &&
              manifest.find("\"detections_csv\"") != std::string::npos &&
              manifest.find("metadata-only; no video-frame mapping") != std::string::npos,
          "final manifest should declare completeness, loss accounting, and the GPU-path contract") &&
      expect(
          pending_manifest_sync < camera_link && camera_link < fast_camera_link && fast_camera_link < detections_link &&
              detections_link < input_directory_sync && input_directory_sync < tracking_link &&
              tracking_link < tracking_commit_sync && tracking_commit_sync < committed_manifest_sync,
          "durability order must commit pending manifest and companion inputs before tracking, then the final manifest") &&
      expect(
          read_file(directory / "play_tracker_source-10.yaml") == read_file(source_config) &&
              read_file(directory / "play_tracker_effective-10.yaml") == read_file(effective_config),
          "base and effective policy configuration must be copied for provenance");

  hm::playtracker::PlayTrackerTelemetryCsv saturated_exporter;
  const absl::Status saturated_start =
      saturated_exporter.Start(directory.string(), source_config.string(), effective_config.string(), 1);
  if (!expect(saturated_start.ok(), saturated_start.ToString()) ||
      !expect(
          saturated_exporter.TryEnqueue(make_policy_sample(true, 100'000)),
          "large leading sample should start the bounded writer") ||
      !expect(saturated_exporter.TryEnqueue(make_policy_sample(true)), "second sample should be retained") ||
      !expect(
          saturated_exporter.TryEnqueue(make_policy_sample(true)), "saturated writer must retain the third sample")) {
    return 1;
  }
  saturated_exporter.MarkRunOutcome(hm::playtracker::TelemetryRunOutcome::kIntentionalStop);
  saturated_exporter.Stop();
  const std::vector<uint64_t> saturated_ids = csv_frame_ids(read_file(directory / "camera-11.csv"));
  const std::string saturated_manifest = read_file(directory / "hstream_telemetry-11.json");
  const bool saturation_valid = expect(
                                    saturated_exporter.queue_full_waits() > 0,
                                    "capacity-one writer should observe queue backpressure") &&
      expect(saturated_exporter.dropped_samples() == 0, "queue saturation must never discard a frame sample") &&
      expect(saturated_ids == std::vector<uint64_t>({1, 2, 3}), "queue saturation must preserve contiguous samples") &&
      expect(csv_frame_ids(read_file(directory / "detections-11.csv")) == saturated_ids &&
                 csv_frame_ids(read_file(directory / "tracking-11.csv")).front() == 1 &&
                 csv_frame_ids(read_file(directory / "tracking-11.csv")).back() == 3,
             "detections, tracking, and camera outputs must remain aligned through queue backpressure") &&
      expect(saturated_manifest.find("\"queue_full_waits\": 0") == std::string::npos &&
                 saturated_manifest.find("\"dropped_samples\": 0") != std::string::npos,
             "manifest should report blocking backpressure without any sample loss");

  hm::playtracker::PlayTrackerTelemetryCsv config_block_exporter;
  const absl::Status config_block_start =
      config_block_exporter.Start(directory.string(), source_config.string(), effective_config.string(), 1);
  if (!expect(config_block_start.ok(), config_block_start.ToString()) ||
      !expect(
          config_block_exporter.TryEnqueue(make_policy_sample(true, 100'000)), "leading sample should be retained") ||
      !expect(config_block_exporter.TryEnqueue(make_policy_sample(true)), "queued sample should be retained") ||
      !expect(
          config_block_exporter.TryRecordConfigEvent({"property", "fixed-edge-rotation-angle", "33", {}, {}}),
          "saturated queue must retain the applied config event") ||
      !expect(config_block_exporter.TryEnqueue(make_policy_sample(true)), "post-change sample should be retained") ||
      !expect(
          config_block_exporter.frame_id_high_watermark() == 4,
          "preserved config event must reserve only its intentional policy-boundary gap")) {
    return 1;
  }
  config_block_exporter.MarkRunOutcome(hm::playtracker::TelemetryRunOutcome::kIntentionalStop);
  config_block_exporter.Stop();
  const std::vector<uint64_t> config_block_ids = csv_frame_ids(read_file(directory / "camera-12.csv"));
  const std::string config_block_events = read_file(directory / "hstream_config_events-12.csv");
  const std::string config_block_manifest = read_file(directory / "hstream_telemetry-12.json");
  const bool config_block_valid =
      expect(
          config_block_ids == std::vector<uint64_t>({1, 2, 4}) &&
              csv_frame_ids(read_file(directory / "detections-12.csv")) == config_block_ids,
          "only the documented policy boundary may create a gap while the queue is saturated") &&
      expect(
          config_block_events.find(",4,property,fixed-edge-rotation-angle,33,") != std::string::npos,
          "the saturated queue must preserve the policy event before the next sample") &&
      expect(
          config_block_manifest.find("\"dropped_config_events\": 0") != std::string::npos &&
              config_block_manifest.find("\"config_events_attempted\": 1") != std::string::npos &&
              config_block_manifest.find("\"config_events_persisted\": 1") != std::string::npos &&
              config_block_manifest.find("\"config_events_lost\": 0") != std::string::npos,
          "manifest must report lossless config-event persistence under backpressure");

  hm::playtracker::PlayTrackerTelemetryCsv event_io_failure_exporter;
  const absl::Status event_io_failure_start =
      event_io_failure_exporter.Start(directory.string(), source_config.string(), effective_config.string(), 8);
  if (!expect(event_io_failure_start.ok(), event_io_failure_start.ToString()) ||
      !expect(event_io_failure_exporter.TryEnqueue(make_policy_sample(true)), "pre-change sample should be accepted")) {
    return 1;
  }
  hm::playtracker::PlayTrackerTelemetryCsvTestPeer::FailNextConfigArtifactWrite(event_io_failure_exporter);
  if (!expect(
          event_io_failure_exporter.TryRecordConfigEvent(
              {"runtime-tuning",
               "runtime-tuning-config-file",
               "runtime.yaml",
               "play_tracker_runtime_tuning",
               "play-tracker:\n  hstream-runtime-tuning:\n    max-speed-x: 34\n"}),
          "config event should enter the queue before the injected artifact writer failure") ||
      !expect(
          event_io_failure_exporter.TryEnqueue(make_policy_sample(true)),
          "post-artifact-failure sample should be accepted") ||
      !expect(
          event_io_failure_exporter.TryRecordConfigEvent(
              {"runtime-tuning",
               "runtime-tuning-config-file",
               "vanished-runtime.yaml",
               "play_tracker_runtime_tuning",
               {}}),
          "event with vanished required provenance should enter the queue") ||
      !expect(
          event_io_failure_exporter.TryEnqueue(make_policy_sample(true)),
          "post-missing-artifact sample should be accepted")) {
    return 1;
  }
  event_io_failure_exporter.MarkRunOutcome(hm::playtracker::TelemetryRunOutcome::kIntentionalStop);
  event_io_failure_exporter.Stop();
  const std::string event_io_failure_events = read_file(directory / "hstream_config_events-13.csv");
  const std::string event_io_failure_manifest = read_file(directory / "hstream_telemetry-13.json");
  const bool event_io_failure_valid =
      expect(
          event_io_failure_events.find("runtime.yaml") == std::string::npos &&
              event_io_failure_events.find("vanished-runtime.yaml") == std::string::npos,
          "failed or missing required artifacts must not advertise their events as persisted") &&
      expect(
          event_io_failure_manifest.find("\"completed\": false") != std::string::npos &&
              event_io_failure_manifest.find("\"eligible_for_training\": false") != std::string::npos &&
              event_io_failure_manifest.find("\"writer_failed\": true") != std::string::npos &&
              event_io_failure_manifest.find("\"frame_id_high_watermark\": 5") != std::string::npos &&
              event_io_failure_manifest.find("\"config_events_attempted\": 2") != std::string::npos &&
              event_io_failure_manifest.find("\"config_events_persisted\": 0") != std::string::npos &&
              event_io_failure_manifest.find("\"config_events_lost\": 2") != std::string::npos,
          "an accepted config event persistence failure must fail the lossless generation") &&
      expect(
          !fs::exists(directory / "tracking-13.csv") && !fs::exists(directory / "detections-13.csv") &&
              !fs::exists(directory / "camera-13.csv") && !fs::exists(directory / "camera_fast-13.csv"),
          "a generation with lost accepted config events must not publish HM training inputs");

  hm::playtracker::PlayTrackerTelemetryCsv aborted_exporter;
  const absl::Status aborted_start =
      aborted_exporter.Start(directory.string(), source_config.string(), effective_config.string(), 8);
  if (!expect(aborted_start.ok(), aborted_start.ToString()) ||
      !expect(aborted_exporter.TryEnqueue(make_policy_sample(true)), "partial failed run should accept a sample")) {
    return 1;
  }
  aborted_exporter.Stop();
  const std::string aborted_manifest = read_file(directory / "hstream_telemetry-14.json");
  const bool aborted_valid = expect(
                                 aborted_manifest.find("\"writer_drained\": true") != std::string::npos &&
                                     aborted_manifest.find("\"run_outcome\": \"incomplete\"") != std::string::npos &&
                                     aborted_manifest.find("\"completed\": false") != std::string::npos &&
                                     aborted_manifest.find("\"eligible_for_training\": false") != std::string::npos,
                                 "draining after a post-start failure must not claim a successful run") &&
      expect(!fs::exists(directory / "tracking-14.csv") && !fs::exists(directory / "camera-14.csv") &&
                 !fs::exists(directory / "camera_fast-14.csv") && !fs::exists(directory / "detections-14.csv") &&
                 !fs::exists(directory / ".tracking-14.csv.partial") &&
                 !fs::exists(directory / ".detections-14.csv.partial") &&
                 !fs::exists(directory / ".camera-14.csv.partial") &&
                 !fs::exists(directory / ".camera_fast-14.csv.partial"),
             "failed partial generation must not become HM's latest eligible training input") &&
      expect(fs::exists(directory / "hstream_frame_index-14.csv"),
             "failed partial generation should retain its audit sidecars");

  hm::playtracker::PlayTrackerTelemetryCsv empty_exporter;
  const absl::Status empty_start =
      empty_exporter.Start(directory.string(), source_config.string(), effective_config.string(), 8);
  if (!expect(empty_start.ok(), empty_start.ToString())) {
    return 1;
  }
  empty_exporter.MarkRunOutcome(hm::playtracker::TelemetryRunOutcome::kIntentionalStop);
  empty_exporter.Stop();
  const std::string empty_manifest = read_file(directory / "hstream_telemetry-15.json");
  const bool empty_valid = expect(
                               empty_manifest.find("\"run_outcome\": \"intentional-stop\"") != std::string::npos &&
                                   empty_manifest.find("\"completed\": true") != std::string::npos &&
                                   empty_manifest.find("\"publication_state\": \"committed\"") != std::string::npos &&
                                   empty_manifest.find("\"eligible_for_training\": false") != std::string::npos,
                               "an intentional but empty run must be complete but remain ineligible") &&
      expect(fs::is_regular_file(directory / "tracking-15.csv") && fs::is_regular_file(directory / "camera-15.csv") &&
                 fs::is_regular_file(directory / "camera_fast-15.csv") &&
                 fs::is_regular_file(directory / "detections-15.csv") &&
                 fs::file_size(directory / "tracking-15.csv") == 0 && fs::file_size(directory / "camera-15.csv") == 0 &&
                 fs::file_size(directory / "camera_fast-15.csv") == 0 &&
                 fs::file_size(directory / "detections-15.csv") == 0 &&
                 !fs::exists(directory / ".tracking-15.csv.partial") &&
                 !fs::exists(directory / ".detections-15.csv.partial") &&
                 !fs::exists(directory / ".camera-15.csv.partial") &&
                 !fs::exists(directory / ".camera_fast-15.csv.partial"),
             "every successful generation must expose all four non-hidden HM CSVs");

  hm::playtracker::PlayTrackerTelemetryCsv publication_conflict_exporter;
  const absl::Status publication_conflict_start =
      publication_conflict_exporter.Start(directory.string(), source_config.string(), effective_config.string(), 8);
  const fs::path publication_conflict_victim = directory / "publication-conflict-victim.txt";
  std::ofstream(publication_conflict_victim) << "preserve-publication-race\n";
  fs::create_symlink(publication_conflict_victim, directory / "tracking-16.csv", error);
  if (!expect(publication_conflict_start.ok(), publication_conflict_start.ToString()) ||
      !expect(!error, "could not create the simulated publication-race symlink") ||
      !expect(
          publication_conflict_exporter.TryEnqueue(make_policy_sample(true)),
          "publication-conflict run should accept a training sample")) {
    return 1;
  }
  publication_conflict_exporter.MarkRunOutcome(hm::playtracker::TelemetryRunOutcome::kIntentionalStop);
  publication_conflict_exporter.Stop();
  const std::string publication_conflict_manifest = read_file(directory / "hstream_telemetry-16.json");
  const bool publication_conflict_valid =
      expect(
          fs::is_symlink(directory / "tracking-16.csv") &&
              read_file(publication_conflict_victim) == "preserve-publication-race\n",
          "no-replace publication must not follow or truncate a concurrently created symlink") &&
      expect(
          !fs::exists(directory / "camera-16.csv") && !fs::exists(directory / "camera_fast-16.csv") &&
              !fs::exists(directory / "detections-16.csv"),
          "a publication conflict must roll back the partially linked camera inputs") &&
      expect(
          !fs::exists(directory / ".tracking-16.csv.partial") &&
              !fs::exists(directory / ".detections-16.csv.partial") &&
              !fs::exists(directory / ".camera-16.csv.partial") &&
              !fs::exists(directory / ".camera_fast-16.csv.partial"),
          "a publication conflict must remove the owned hidden staging inputs") &&
      expect(
          publication_conflict_manifest.find("\"writer_failed\": true") != std::string::npos &&
              publication_conflict_manifest.find("\"completed\": false") != std::string::npos &&
              publication_conflict_manifest.find("\"eligible_for_training\": false") != std::string::npos,
          "a publication conflict must leave the generation explicitly ineligible");

  hm::playtracker::PlayTrackerTelemetryCsv failed_outcome_exporter;
  const absl::Status failed_outcome_start =
      failed_outcome_exporter.Start(directory.string(), source_config.string(), effective_config.string(), 8);
  if (!expect(failed_outcome_start.ok(), failed_outcome_start.ToString()) ||
      !expect(failed_outcome_exporter.TryEnqueue(make_policy_sample(true)), "failed run should accept a sample")) {
    return 1;
  }
  failed_outcome_exporter.MarkRunOutcome(hm::playtracker::TelemetryRunOutcome::kEndOfStream);
  failed_outcome_exporter.MarkRunOutcome(hm::playtracker::TelemetryRunOutcome::kFailed);
  failed_outcome_exporter.Stop();
  const std::string failed_outcome_manifest = read_file(directory / "hstream_telemetry-17.json");
  const bool failed_outcome_valid =
      expect(
          failed_outcome_manifest.find("\"run_outcome\": \"failed\"") != std::string::npos &&
              failed_outcome_manifest.find("\"completed\": false") != std::string::npos &&
              failed_outcome_manifest.find("\"eligible_for_training\": false") != std::string::npos,
          "a fatal pipeline result must downgrade an earlier element-local EOS") &&
      expect(
          !fs::exists(directory / "tracking-17.csv") && !fs::exists(directory / "camera-17.csv") &&
              !fs::exists(directory / "camera_fast-17.csv") && !fs::exists(directory / "detections-17.csv"),
          "a failed pipeline outcome must not publish HM training inputs");

  hm::playtracker::PlayTrackerTelemetryCsv final_sync_failure_exporter;
  const absl::Status final_sync_failure_start =
      final_sync_failure_exporter.Start(directory.string(), source_config.string(), effective_config.string(), 8);
  if (!expect(final_sync_failure_start.ok(), final_sync_failure_start.ToString()) ||
      !expect(
          final_sync_failure_exporter.TryEnqueue(make_policy_sample(true)),
          "final-sync-failure run should accept a sample")) {
    return 1;
  }
  final_sync_failure_exporter.MarkRunOutcome(hm::playtracker::TelemetryRunOutcome::kIntentionalStop);
  hm::playtracker::PlayTrackerTelemetryCsvTestPeer::FailFirstStagedSync(final_sync_failure_exporter);
  final_sync_failure_exporter.Stop();
  const std::string final_sync_failure_manifest = read_file(directory / "hstream_telemetry-18.json");
  const bool final_sync_failure_valid =
      expect(
          final_sync_failure_manifest.find("\"samples_buffered\": 1") != std::string::npos &&
              final_sync_failure_manifest.find("\"training_samples_buffered\": 1") != std::string::npos &&
              final_sync_failure_manifest.find("\"samples_persisted\": 0") != std::string::npos &&
              final_sync_failure_manifest.find("\"training_samples_persisted\": 0") != std::string::npos &&
              final_sync_failure_manifest.find("\"writer_failed\": true") != std::string::npos &&
              final_sync_failure_manifest.find("\"publication_state\": \"pending\"") != std::string::npos,
          "ENOSPC during final durability sync must not claim buffered rows were persisted") &&
      expect(
          !fs::exists(directory / "tracking-18.csv") && !fs::exists(directory / "camera-18.csv") &&
              !fs::exists(directory / "camera_fast-18.csv") && !fs::exists(directory / "detections-18.csv"),
          "a failed final durability sync must not publish training inputs");

  hm::playtracker::PlayTrackerTelemetryCsv camera_commit_failure_exporter;
  const absl::Status camera_commit_failure_start =
      camera_commit_failure_exporter.Start(directory.string(), source_config.string(), effective_config.string(), 8);
  if (!expect(camera_commit_failure_start.ok(), camera_commit_failure_start.ToString()) ||
      !expect(
          camera_commit_failure_exporter.TryEnqueue(make_policy_sample(true)),
          "camera-commit-failure run should accept a sample")) {
    return 1;
  }
  camera_commit_failure_exporter.MarkRunOutcome(hm::playtracker::TelemetryRunOutcome::kIntentionalStop);
  hm::playtracker::PlayTrackerTelemetryCsvTestPeer::FailSync(camera_commit_failure_exporter, "fsync:directory:inputs");
  camera_commit_failure_exporter.Stop();
  const std::string camera_commit_failure_manifest = read_file(directory / "hstream_telemetry-19.json");
  const std::vector<std::string> camera_failure_events =
      hm::playtracker::PlayTrackerTelemetryCsvTestPeer::DurabilityEvents(camera_commit_failure_exporter);
  const bool camera_commit_failure_valid =
      expect(
          camera_commit_failure_manifest.find("\"writer_failed\": true") != std::string::npos &&
              camera_commit_failure_manifest.find("\"publication_state\": \"pending\"") != std::string::npos &&
              camera_commit_failure_manifest.find("\"eligible_for_training\": false") != std::string::npos,
          "camera-link durability failure must retain a pending ineligible manifest") &&
      expect(
          !fs::exists(directory / "tracking-19.csv") && !fs::exists(directory / "camera-19.csv") &&
              !fs::exists(directory / "camera_fast-19.csv") && !fs::exists(directory / "detections-19.csv") &&
              event_position(camera_failure_events, "link:tracking-19.csv") == camera_failure_events.size(),
          "tracking must not be linked when the camera directory commit fails");

  hm::playtracker::PlayTrackerTelemetryCsv tracking_commit_failure_exporter;
  const absl::Status tracking_commit_failure_start =
      tracking_commit_failure_exporter.Start(directory.string(), source_config.string(), effective_config.string(), 8);
  if (!expect(tracking_commit_failure_start.ok(), tracking_commit_failure_start.ToString()) ||
      !expect(
          tracking_commit_failure_exporter.TryEnqueue(make_policy_sample(true)),
          "tracking-commit-failure run should accept a sample")) {
    return 1;
  }
  tracking_commit_failure_exporter.MarkRunOutcome(hm::playtracker::TelemetryRunOutcome::kIntentionalStop);
  hm::playtracker::PlayTrackerTelemetryCsvTestPeer::FailDirectorySyncsFrom(
      tracking_commit_failure_exporter, "fsync:directory:tracking-commit");
  tracking_commit_failure_exporter.Stop();
  const std::string tracking_commit_failure_manifest = read_file(directory / "hstream_telemetry-20.json");
  const std::vector<std::string> tracking_failure_events =
      hm::playtracker::PlayTrackerTelemetryCsvTestPeer::DurabilityEvents(tracking_commit_failure_exporter);
  const size_t tracking_failure_link = event_position(tracking_failure_events, "link:tracking-20.csv");
  const size_t tracking_failure_commit = event_position(tracking_failure_events, "fsync:directory:tracking-commit");
  const size_t tracking_failure_unlink = event_position(tracking_failure_events, "unlink:tracking-20.csv");
  const size_t tracking_failure_rollback = event_position(tracking_failure_events, "fsync:directory:tracking-rollback");
  const bool tracking_commit_failure_valid =
      expect(
          tracking_commit_failure_manifest.find("\"writer_failed\": true") != std::string::npos &&
              tracking_commit_failure_manifest.find("\"publication_state\": \"pending\"") != std::string::npos &&
              tracking_commit_failure_manifest.find("\"eligible_for_training\": false") != std::string::npos,
          "uncertain tracking-link durability must retain a pending ineligible manifest") &&
      expect(
          !fs::exists(directory / "tracking-20.csv") && fs::is_regular_file(directory / "camera-20.csv") &&
              fs::is_regular_file(directory / "camera_fast-20.csv") &&
              fs::is_regular_file(directory / "detections-20.csv") &&
              fs::is_regular_file(directory / "tracking-10.csv") && fs::is_regular_file(directory / "camera-10.csv"),
          "tracking must be retracted before camera cleanup, leaving the prior committed generation discoverable") &&
      expect(
          tracking_failure_link < tracking_failure_commit && tracking_failure_commit < tracking_failure_unlink &&
              tracking_failure_unlink < tracking_failure_rollback &&
              event_position(tracking_failure_events, "unlink:camera-20.csv") == tracking_failure_events.size() &&
              event_position(tracking_failure_events, "unlink:camera_fast-20.csv") == tracking_failure_events.size() &&
              event_position(tracking_failure_events, "unlink:detections-20.csv") == tracking_failure_events.size(),
          "failed tracking rollback fsync must retain durable companion inputs for crash-safe recovery");

  const fs::path atomic_manifest_directory = directory / "atomic-manifest";
  hm::playtracker::PlayTrackerTelemetryCsv atomic_manifest_exporter;
  const absl::Status atomic_manifest_start = atomic_manifest_exporter.Start(
      atomic_manifest_directory.string(), source_config.string(), effective_config.string(), 8);
  const fs::path atomic_manifest_path = atomic_manifest_directory / "hstream_telemetry.json";
  const std::string initial_atomic_manifest = read_file(atomic_manifest_path);
  const std::string pre_rename_phase = "fsync:manifest:test-pre-rename";
  hm::playtracker::PlayTrackerTelemetryCsvTestPeer::FailSync(atomic_manifest_exporter, pre_rename_phase);
  const bool pre_rename_result = hm::playtracker::PlayTrackerTelemetryCsvTestPeer::RewriteManifestState(
      atomic_manifest_exporter, /*committed=*/true, pre_rename_phase);
  const std::string after_pre_rename_failure = read_file(atomic_manifest_path);
  const std::string post_rename_phase = "fsync:manifest:test-post-rename";
  const std::string post_rename_directory_sync = "fsync:directory:manifest:" + post_rename_phase;
  hm::playtracker::PlayTrackerTelemetryCsvTestPeer::FailSync(atomic_manifest_exporter, post_rename_directory_sync);
  const bool post_rename_result = hm::playtracker::PlayTrackerTelemetryCsvTestPeer::RewriteManifestState(
      atomic_manifest_exporter, /*committed=*/true, post_rename_phase);
  const std::string after_post_rename_failure = read_file(atomic_manifest_path);
  const bool atomic_manifest_retry = hm::playtracker::PlayTrackerTelemetryCsvTestPeer::RewriteManifestState(
      atomic_manifest_exporter, /*committed=*/true, "fsync:manifest:test-retry");
  const std::vector<std::string> atomic_manifest_events =
      hm::playtracker::PlayTrackerTelemetryCsvTestPeer::DurabilityEvents(atomic_manifest_exporter);
  atomic_manifest_exporter.Stop();
  const bool atomic_manifest_valid = expect(atomic_manifest_start.ok(), atomic_manifest_start.ToString()) &&
      expect(!pre_rename_result, "injected pre-rename manifest fsync failure must reject the rewrite") &&
      expect(after_pre_rename_failure == initial_atomic_manifest,
             "pre-rename failure must preserve the previous complete manifest byte-for-byte") &&
      expect(!post_rename_result, "injected post-rename directory fsync failure must report failure") &&
      expect(after_post_rename_failure.find("\"publication_state\": \"committed\"") != std::string::npos &&
                 !after_post_rename_failure.empty() && after_post_rename_failure.front() == '{' &&
                 after_post_rename_failure.size() >= 2 &&
                 after_post_rename_failure.compare(after_post_rename_failure.size() - 2, 2, "}\n") == 0,
             "post-rename failure must expose a complete old-or-new manifest, never partial JSON") &&
      expect(atomic_manifest_retry, "manifest rewrite should be retryable after a directory fsync failure") &&
      expect(event_position(atomic_manifest_events, "rename:manifest:" + post_rename_phase) <
                 event_position(atomic_manifest_events, post_rename_directory_sync),
             "manifest replacement must rename only a durable temp inode and then fsync the directory");

  const fs::path descriptor_directory = directory / "descriptor-bound";
  hm::playtracker::PlayTrackerTelemetryCsv descriptor_bound_exporter;
  const absl::Status descriptor_bound_start = descriptor_bound_exporter.Start(
      descriptor_directory.string(), source_config.string(), effective_config.string(), 256);
  const size_t initial_open_artifact_descriptors =
      hm::playtracker::PlayTrackerTelemetryCsvTestPeer::OpenArtifactDescriptorCount(descriptor_bound_exporter);
  bool descriptor_events_accepted = descriptor_bound_start.ok();
  for (size_t event = 0; event < 128 && descriptor_events_accepted; ++event) {
    descriptor_events_accepted = descriptor_bound_exporter.TryRecordConfigEvent(
        {"runtime-tuning",
         "runtime-tuning-config-file",
         "runtime.yaml",
         "play_tracker_runtime_tuning",
         "play-tracker:\n  hstream-runtime-tuning:\n    max-speed-x: " + std::to_string(event) + "\n"});
  }
  const bool descriptor_events_drained = descriptor_events_accepted &&
      hm::playtracker::PlayTrackerTelemetryCsvTestPeer::WaitUntilConfigEventsBuffered(descriptor_bound_exporter, 128);
  const size_t final_open_artifact_descriptors =
      hm::playtracker::PlayTrackerTelemetryCsvTestPeer::OpenArtifactDescriptorCount(descriptor_bound_exporter);
  descriptor_bound_exporter.MarkRunOutcome(hm::playtracker::TelemetryRunOutcome::kIntentionalStop);
  descriptor_bound_exporter.Stop();
  const bool descriptor_bound_valid = expect(descriptor_bound_start.ok(), descriptor_bound_start.ToString()) &&
      expect(descriptor_events_accepted, "repeated runtime provenance events should fit the bounded queue") &&
      expect(descriptor_events_drained, "writer should durably process every repeated runtime provenance event") &&
      expect(final_open_artifact_descriptors == initial_open_artifact_descriptors,
             "durable runtime provenance artifacts must not retain one open descriptor per event");

  const fs::path atomic_directory = directory / "atomic-reservation";
  fs::create_directories(atomic_directory, error);
  const fs::path symlink_victim = atomic_directory / "do-not-truncate.txt";
  std::ofstream(symlink_victim) << "preserve-concurrent-data\n";
  fs::create_symlink(symlink_victim, atomic_directory / "tracking.csv", error);
  hm::playtracker::PlayTrackerTelemetryCsv concurrent_first;
  hm::playtracker::PlayTrackerTelemetryCsv concurrent_second;
  absl::Status concurrent_first_start;
  absl::Status concurrent_second_start;
  std::thread first_starter([&]() {
    concurrent_first_start =
        concurrent_first.Start(atomic_directory.string(), source_config.string(), effective_config.string(), 8);
  });
  std::thread second_starter([&]() {
    concurrent_second_start =
        concurrent_second.Start(atomic_directory.string(), source_config.string(), effective_config.string(), 8);
  });
  first_starter.join();
  second_starter.join();
  const bool concurrent_started = concurrent_first_start.ok() && concurrent_second_start.ok();
  if (concurrent_started) {
    concurrent_first.TryEnqueue(make_policy_sample(true));
    concurrent_second.TryEnqueue(make_policy_sample(true));
    concurrent_first.MarkRunOutcome(hm::playtracker::TelemetryRunOutcome::kIntentionalStop);
    concurrent_second.MarkRunOutcome(hm::playtracker::TelemetryRunOutcome::kIntentionalStop);
    concurrent_first.Stop();
    concurrent_second.Stop();
  }
  const bool atomic_reservation_valid = expect(
                                            concurrent_started,
                                            "concurrent exporters should reserve distinct complete generations") &&
      expect(concurrent_first.output_manifest() != concurrent_second.output_manifest(),
             "directory transaction must assign concurrent exporters different generations") &&
      expect(fs::is_regular_file(atomic_directory / "tracking-1.csv") &&
                 fs::is_regular_file(atomic_directory / "tracking-2.csv") &&
                 fs::is_regular_file(atomic_directory / "detections-1.csv") &&
                 fs::is_regular_file(atomic_directory / "detections-2.csv"),
             "both concurrent generations should publish their exclusively reserved HM files") &&
      expect(fs::is_symlink(atomic_directory / "tracking.csv") &&
                 read_file(symlink_victim) == "preserve-concurrent-data\n",
             "pre-existing HM symlink and its target must never be followed or truncated");

  const fs::path failed_directory = directory / "failed-start";
  hm::playtracker::PlayTrackerTelemetryCsv failed_exporter;
  const absl::Status failed_start = failed_exporter.Start(
      failed_directory.string(), (directory / "missing-source.yaml").string(), effective_config.string());
  bool failed_directory_empty = true;
  if (fs::exists(failed_directory)) {
    failed_directory_empty = fs::directory_iterator(failed_directory) == fs::directory_iterator();
  }
  const bool startup_cleanup_valid =
      expect(!failed_start.ok(), "unreadable startup config should reject telemetry export") &&
      expect(failed_directory_empty, "failed startup must not leave empty generation artifacts");

  fs::remove_all(directory, error);
  return valid && saturation_valid && config_block_valid && event_io_failure_valid && aborted_valid && empty_valid &&
          publication_conflict_valid && failed_outcome_valid && final_sync_failure_valid &&
          camera_commit_failure_valid && tracking_commit_failure_valid && atomic_manifest_valid &&
          descriptor_bound_valid && atomic_reservation_valid && startup_cleanup_valid
      ? 0
      : 1;
}
