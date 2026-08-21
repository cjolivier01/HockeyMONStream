#include "hstream/src/gst-plugins/gst-videoprep/playtracker/PlayTrackerTelemetryCsv.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace hm::playtracker {

class PlayTrackerTelemetryCsvTestPeer {
 public:
  static std::pair<bool, bool> EnqueueThenRecordWhileWriterExcluded(
      PlayTrackerTelemetryCsv& exporter,
      TelemetrySample sample,
      TelemetryConfigEvent event) {
    std::lock_guard<std::mutex> lock(exporter.mutex_);
    const bool sample_accepted = exporter.TryEnqueueLocked(std::move(sample));
    const bool event_accepted = exporter.TryRecordConfigEventLocked(std::move(event));
    return {sample_accepted, event_accepted};
  }

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

hm::playtracker::TelemetrySample make_policy_sample(bool with_track, size_t extra_tracks = 0) {
  hm::playtracker::TelemetrySample sample;
  sample.width = 3840;
  sample.height = 1080;
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
              !fs::exists(directory / "camera_fast-10.csv"),
          "an active run must not expose HM-discoverable training filenames")) {
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
  const std::string camera = read_file(directory / "camera-10.csv");
  const std::string camera_fast = read_file(directory / "camera_fast-10.csv");
  const std::string frame_index = read_file(directory / "hstream_frame_index-10.csv");
  const std::string config_events = read_file(directory / "hstream_config_events-10.csv");
  const std::string manifest = read_file(directory / "hstream_telemetry-10.json");

  const bool valid = expect(
                         read_file(directory / "tracking.csv") == "preserve-existing-hm-data\n",
                         "pre-existing HM tracking data must remain byte-for-byte intact") &&
      expect(tracking_fields.size() == 13, "tracking CSV must use HM's current 13-column headerless schema") &&
      expect(tracking_fields[0] == "1" && tracking_fields[1] == "88" && tracking_fields[8] == "-1" &&
                 tracking_fields[9] == "{}" && tracking_fields[10].empty() && tracking_fields[11] == "0" &&
                 tracking_fields[12] == "-1",
             "tracking CSV identity and optional compatibility columns should match HM") &&
      expect(camera.rfind("1,3,4,500,250", 0) == 0, "camera.csv should contain the follower/Program policy action") &&
      expect(camera_fast.rfind("1,1,2,300,150", 0) == 0, "camera_fast.csv should contain the fast policy action") &&
      expect(csv_frame_ids(tracking) == std::vector<uint64_t>({1, 6}),
             "seek and trackless intervals must remain visible as numeric gaps in tracking.csv") &&
      expect(csv_frame_ids(camera) == std::vector<uint64_t>({1, 4, 6}),
             "trackless samples should keep their camera action without collapsing the timeline") &&
      expect(frame_index.find("1,7,991,2,1234,17490000000,42,3,3840,1080,1,1") != std::string::npos,
             "frame sidecar should preserve source, native frame, timestamps, seek epoch, and dimensions") &&
      expect(config_events.find("runtime-tuning,max-speed-x,24,runtime-10-1.yaml") != std::string::npos &&
                 read_file(directory / "runtime-10-1.yaml").find("max-speed-x: 24") != std::string::npos,
             "runtime policy changes should retain exact configuration artifacts") &&
      expect(config_events.find("seek,flush-stop,1,") != std::string::npos &&
                 config_events.find(",4,seek,flush-stop,1,") != std::string::npos,
             "seek config event should identify the first post-gap sample boundary") &&
      expect(config_events.find(",6,property,fixed-edge-rotation-angle,31,") != std::string::npos,
             "policy config event should be ordered at the next sample boundary") &&
      expect(manifest.find("\"completed\": true") != std::string::npos &&
                 manifest.find("\"eligible_for_training\": true") != std::string::npos &&
                 manifest.find("\"frame_id_high_watermark\": 6") != std::string::npos &&
                 manifest.find("\"samples_attempted\": 3") != std::string::npos &&
                 manifest.find("\"discontinuity_gaps\": 3") != std::string::npos &&
                 manifest.find("\"config_events_attempted\": 3") != std::string::npos &&
                 manifest.find("\"config_events_persisted\": 3") != std::string::npos &&
                 manifest.find("\"config_events_lost\": 0") != std::string::npos &&
                 manifest.find("\"dropped_samples\": 0") != std::string::npos &&
                 manifest.find("metadata-only; no video-frame mapping") != std::string::npos,
             "final manifest should declare completeness, loss accounting, and the GPU-path contract") &&
      expect(read_file(directory / "play_tracker_source-10.yaml") == read_file(source_config) &&
                 read_file(directory / "play_tracker_effective-10.yaml") == read_file(effective_config),
             "base and effective policy configuration must be copied for provenance");

  hm::playtracker::PlayTrackerTelemetryCsv overflow_exporter;
  const absl::Status overflow_start =
      overflow_exporter.Start(directory.string(), source_config.string(), effective_config.string(), 1);
  if (!expect(overflow_start.ok(), overflow_start.ToString()) ||
      !expect(
          overflow_exporter.TryEnqueue(make_policy_sample(true, 100'000)),
          "large leading sample should start the bounded writer")) {
    return 1;
  }

  bool saw_drop = false;
  uint64_t first_dropped_id = 0;
  uint64_t accepted_after_drop = 0;
  for (size_t attempt = 0; attempt < 1'000'000 && accepted_after_drop == 0; ++attempt) {
    const bool accepted = overflow_exporter.TryEnqueue(make_policy_sample(true));
    const uint64_t frame_id = overflow_exporter.frame_id_high_watermark();
    if (!accepted && !saw_drop) {
      saw_drop = true;
      first_dropped_id = frame_id;
    } else if (accepted && saw_drop) {
      accepted_after_drop = frame_id;
    }
    if (saw_drop && accepted_after_drop == 0) {
      std::this_thread::yield();
    }
  }
  overflow_exporter.MarkRunOutcome(hm::playtracker::TelemetryRunOutcome::kIntentionalStop);
  overflow_exporter.Stop();
  const uint64_t overflow_dropped = overflow_exporter.dropped_samples();
  const std::vector<uint64_t> overflow_camera_ids = csv_frame_ids(read_file(directory / "camera-11.csv"));
  const std::set<uint64_t> overflow_camera_id_set(overflow_camera_ids.begin(), overflow_camera_ids.end());
  const std::string overflow_manifest = read_file(directory / "hstream_telemetry-11.json");
  const bool overflow_valid = expect(saw_drop, "bounded writer regression must force at least one complete drop") &&
      expect(overflow_dropped > 0, "overflow counter should retain the number of missing attempted samples") &&
      expect(accepted_after_drop > first_dropped_id, "writer should recover and accept a later sample") &&
      expect(overflow_camera_id_set.count(first_dropped_id) == 0 &&
                 overflow_camera_id_set.count(accepted_after_drop) == 1,
             "a dropped attempt must leave a numeric gap before the later accepted sample") &&
      expect(overflow_manifest.find("\"dropped_samples\": " + std::to_string(overflow_dropped)) != std::string::npos,
             "overflow manifest should report dropped samples");

  hm::playtracker::PlayTrackerTelemetryCsv config_drop_exporter;
  const absl::Status config_drop_start =
      config_drop_exporter.Start(directory.string(), source_config.string(), effective_config.string(), 1);
  if (!expect(config_drop_start.ok(), config_drop_start.ToString())) {
    return 1;
  }
  const auto [saturated_sample_accepted, config_event_accepted] =
      hm::playtracker::PlayTrackerTelemetryCsvTestPeer::EnqueueThenRecordWhileWriterExcluded(
          config_drop_exporter, make_policy_sample(true), {"property", "fixed-edge-rotation-angle", "33", {}, {}});
  if (!expect(saturated_sample_accepted, "capacity-one queue should accept the pre-change sample") ||
      !expect(!config_event_accepted, "saturated queue should reject the applied config event") ||
      !expect(
          hm::playtracker::PlayTrackerTelemetryCsvTestPeer::WaitUntilQueueEmpty(config_drop_exporter),
          "writer should drain the deliberately saturated queue") ||
      !expect(config_drop_exporter.TryEnqueue(make_policy_sample(true)), "post-change sample should be accepted") ||
      !expect(
          config_drop_exporter.frame_id_high_watermark() == 3,
          "dropped config event must reserve frame ID 2 between pre/post-change samples")) {
    return 1;
  }
  config_drop_exporter.MarkRunOutcome(hm::playtracker::TelemetryRunOutcome::kIntentionalStop);
  config_drop_exporter.Stop();
  const std::vector<uint64_t> config_drop_tracking_ids = csv_frame_ids(read_file(directory / "tracking-12.csv"));
  const std::vector<uint64_t> config_drop_camera_ids = csv_frame_ids(read_file(directory / "camera-12.csv"));
  const std::string config_drop_events = read_file(directory / "hstream_config_events-12.csv");
  const std::string config_drop_manifest = read_file(directory / "hstream_telemetry-12.json");
  const bool config_drop_valid =
      expect(
          config_drop_tracking_ids == std::vector<uint64_t>({1, 3}) &&
              config_drop_camera_ids == std::vector<uint64_t>({1, 3}),
          "HM training rows must contain a numeric gap across the unrecorded policy change") &&
      expect(
          config_drop_events.find("fixed-edge-rotation-angle") == std::string::npos,
          "the saturated queue must not claim it preserved the dropped config event") &&
      expect(
          config_drop_manifest.find("\"frame_id_high_watermark\": 3") != std::string::npos &&
              config_drop_manifest.find("\"samples_attempted\": 2") != std::string::npos &&
              config_drop_manifest.find("\"discontinuity_gaps\": 1") != std::string::npos &&
              config_drop_manifest.find("\"config_event_discontinuity_gaps\": 1") != std::string::npos &&
              config_drop_manifest.find("\"dropped_config_events\": 1") != std::string::npos &&
              config_drop_manifest.find("\"config_events_attempted\": 1") != std::string::npos &&
              config_drop_manifest.find("\"config_events_persisted\": 0") != std::string::npos &&
              config_drop_manifest.find("\"config_events_lost\": 1") != std::string::npos,
          "manifest must account for the dropped config event and its protective discontinuity");

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
  const bool event_io_failure_valid = expect(
                                          csv_frame_ids(read_file(directory / "tracking-13.csv")) ==
                                                  std::vector<uint64_t>({1, 3, 5}) &&
                                              csv_frame_ids(read_file(directory / "camera-13.csv")) ==
                                                  std::vector<uint64_t>({1, 3, 5}),
                                          "async config-event I/O loss must retain a numeric HM training gap") &&
      expect(event_io_failure_events.find("runtime.yaml") == std::string::npos &&
                 event_io_failure_events.find("vanished-runtime.yaml") == std::string::npos,
             "failed or missing required artifacts must not advertise their events as persisted") &&
      expect(event_io_failure_manifest.find("\"completed\": true") != std::string::npos &&
                 event_io_failure_manifest.find("\"eligible_for_training\": true") != std::string::npos &&
                 event_io_failure_manifest.find("\"writer_failed\": false") != std::string::npos &&
                 event_io_failure_manifest.find("\"frame_id_high_watermark\": 5") != std::string::npos &&
                 event_io_failure_manifest.find("\"config_events_attempted\": 2") != std::string::npos &&
                 event_io_failure_manifest.find("\"config_events_persisted\": 0") != std::string::npos &&
                 event_io_failure_manifest.find("\"config_events_lost\": 2") != std::string::npos,
             "manifest must distinguish protected artifact/event loss from core writer failure");

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
                 !fs::exists(directory / "camera_fast-14.csv") && !fs::exists(directory / ".tracking-14.csv.partial") &&
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
                                   empty_manifest.find("\"completed\": false") != std::string::npos &&
                                   empty_manifest.find("\"eligible_for_training\": false") != std::string::npos,
                               "an intentional but empty run must remain ineligible") &&
      expect(!fs::exists(directory / "tracking-15.csv") && !fs::exists(directory / "camera-15.csv") &&
                 !fs::exists(directory / "camera_fast-15.csv") && !fs::exists(directory / ".tracking-15.csv.partial") &&
                 !fs::exists(directory / ".camera-15.csv.partial") &&
                 !fs::exists(directory / ".camera_fast-15.csv.partial"),
             "empty generation must not shadow the latest usable HM training files");

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
          !fs::exists(directory / "camera-16.csv") && !fs::exists(directory / "camera_fast-16.csv"),
          "a publication conflict must roll back the partially linked camera inputs") &&
      expect(
          !fs::exists(directory / ".tracking-16.csv.partial") && !fs::exists(directory / ".camera-16.csv.partial") &&
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
              !fs::exists(directory / "camera_fast-17.csv"),
          "a failed pipeline outcome must not publish HM training inputs");

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
                 fs::is_regular_file(atomic_directory / "tracking-2.csv"),
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
  return valid && overflow_valid && config_drop_valid && event_io_failure_valid && aborted_valid && empty_valid &&
          publication_conflict_valid && failed_outcome_valid && atomic_reservation_valid && startup_cleanup_valid
      ? 0
      : 1;
}
