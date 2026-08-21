#include "hstream/src/gst-plugins/gst-videoprep/playtracker/PlayTrackerTelemetryCsv.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
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
      expect(frame_index.find("1,7,991,2,1234,17490000000,42,3,3840,1080,1,1") != std::string::npos,
             "frame sidecar should preserve source, native frame, timestamps, seek epoch, and dimensions") &&
      expect(config_events.find("runtime-tuning,max-speed-x,24,runtime-10-1.yaml") != std::string::npos &&
                 read_file(directory / "runtime-10-1.yaml").find("max-speed-x: 24") != std::string::npos,
             "runtime policy changes should retain exact configuration artifacts") &&
      expect(manifest.find("\"completed\": true") != std::string::npos &&
                 manifest.find("\"dropped_samples\": 0") != std::string::npos &&
                 manifest.find("metadata-only; no video-frame mapping") != std::string::npos,
             "final manifest should declare completeness, loss accounting, and the GPU-path contract") &&
      expect(read_file(directory / "play_tracker_source-10.yaml") == read_file(source_config) &&
                 read_file(directory / "play_tracker_effective-10.yaml") == read_file(effective_config),
             "base and effective policy configuration must be copied for provenance");

  fs::remove_all(directory, error);
  return valid ? 0 : 1;
}
