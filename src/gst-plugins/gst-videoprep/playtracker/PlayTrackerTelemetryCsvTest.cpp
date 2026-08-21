#include "hstream/src/gst-plugins/gst-videoprep/playtracker/PlayTrackerTelemetryCsv.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

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
  const fs::path directory = fs::temp_directory_path() /
      ("hstream-playtracker-telemetry-test-" + std::to_string(static_cast<uint64_t>(::getpid())));
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
      expect(csv_frame_ids(tracking) == std::vector<uint64_t>({1, 4}),
             "seek and trackless intervals must remain visible as numeric gaps in tracking.csv") &&
      expect(csv_frame_ids(camera) == std::vector<uint64_t>({1, 3, 4}),
             "trackless samples should keep their camera action without collapsing the timeline") &&
      expect(frame_index.find("1,7,991,2,1234,17490000000,42,3,3840,1080,1,1") != std::string::npos,
             "frame sidecar should preserve source, native frame, timestamps, seek epoch, and dimensions") &&
      expect(config_events.find("runtime-tuning,max-speed-x,24,runtime-10-1.yaml") != std::string::npos &&
                 read_file(directory / "runtime-10-1.yaml").find("max-speed-x: 24") != std::string::npos,
             "runtime policy changes should retain exact configuration artifacts") &&
      expect(config_events.find("seek,flush-stop,1,") != std::string::npos &&
                 config_events.find(",3,seek,flush-stop,1,") != std::string::npos,
             "seek config event should identify the first post-gap sample boundary") &&
      expect(config_events.find(",4,property,fixed-edge-rotation-angle,31,") != std::string::npos,
             "policy config event should be ordered at the next sample boundary") &&
      expect(manifest.find("\"completed\": true") != std::string::npos &&
                 manifest.find("\"frame_id_high_watermark\": 4") != std::string::npos &&
                 manifest.find("\"samples_attempted\": 3") != std::string::npos &&
                 manifest.find("\"discontinuity_gaps\": 1") != std::string::npos &&
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
  return valid && overflow_valid && startup_cleanup_valid ? 0 : 1;
}
