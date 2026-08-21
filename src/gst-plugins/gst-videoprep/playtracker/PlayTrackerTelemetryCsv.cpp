#include "hstream/src/gst-plugins/gst-videoprep/playtracker/PlayTrackerTelemetryCsv.h"

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace hm::playtracker {
namespace {

namespace fs = std::filesystem;

std::string utc_now() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::string csv_string(const std::string& value) {
  if (value.find_first_of(",\"\r\n") == std::string::npos) {
    return value;
  }
  std::string quoted;
  quoted.reserve(value.size() + 2);
  quoted.push_back('"');
  for (const char c : value) {
    if (c == '"') {
      quoted.push_back('"');
    }
    quoted.push_back(c);
  }
  quoted.push_back('"');
  return quoted;
}

std::string json_string(const std::string& value) {
  std::ostringstream out;
  out << '"';
  for (const unsigned char c : value) {
    switch (c) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\b':
        out << "\\b";
        break;
      case '\f':
        out << "\\f";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (c < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(c) << std::dec
              << std::setfill(' ');
        } else {
          out << static_cast<char>(c);
        }
    }
  }
  out << '"';
  return out.str();
}

std::string suffixed_name(const std::string& stem, const std::string& suffix, const std::string& extension) {
  return stem + suffix + extension;
}

bool write_file(const fs::path& path, const std::string& contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  return output.good();
}

template <typename T>
void write_optional(std::ostream& output, const std::optional<T>& value) {
  if (value.has_value()) {
    output << *value;
  }
}

void write_camera_row(std::ostream& output, uint64_t sample_id, const TelemetryBox& box) {
  output << sample_id << ',' << std::setprecision(9) << box.left << ',' << box.top << ',' << box.width << ','
         << box.height << '\n';
}

} // namespace

std::string ReadTelemetryConfigArtifact(const std::string& path) {
  if (path.empty()) {
    return {};
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

PlayTrackerTelemetryCsv::~PlayTrackerTelemetryCsv() {
  Stop();
}

absl::Status PlayTrackerTelemetryCsv::Start(
    const std::string& output_directory,
    const std::string& source_config_file,
    const std::string& effective_config_file,
    size_t queue_capacity) {
  Stop();
  if (output_directory.empty()) {
    return absl::InvalidArgumentError("playtracker telemetry output directory is empty");
  }
  if (queue_capacity == 0) {
    return absl::InvalidArgumentError("playtracker telemetry queue capacity must be positive");
  }

  queue_capacity_ = queue_capacity;
  next_sample_id_.store(0, std::memory_order_release);
  dropped_samples_.store(0, std::memory_order_release);
  dropped_config_events_.store(0, std::memory_order_release);
  config_event_sequence_ = 0;
  config_artifact_sequence_ = 0;
  writer_failed_ = false;
  stopping_ = false;
  started_utc_ = utc_now();
  source_config_path_ = source_config_file;
  effective_config_path_ = effective_config_file;
  const absl::Status open_status = OpenOutputs(output_directory, source_config_file, effective_config_file);
  if (!open_status.ok()) {
    CloseOutputs();
    return open_status;
  }

  frame_index_ << "Sample,SourceID,SourceFrame,DecodedSourceID,DecodedSequence,PTS_NS,NTP_NS,SeekEpoch,Width,Height,"
                  "TrackCount,HasCamera\n";
  config_events_ << "Event,SampleBoundary,Kind,Key,Value,Artifact\n";
  config_events_ << "0,1,base-config,config-file," << csv_string(source_config_file) << ','
                 << csv_string(source_config_filename_) << '\n';
  if (!tracking_ || !camera_ || !camera_fast_ || !frame_index_ || !config_events_) {
    CloseOutputs();
    return absl::InternalError("could not initialize playtracker telemetry CSV headers");
  }

  WriteManifest(false);
  if (writer_failed_) {
    CloseOutputs();
    return absl::InternalError("could not write initial playtracker telemetry manifest");
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = true;
  }
  writer_thread_ = std::thread(&PlayTrackerTelemetryCsv::WriterLoop, this);
  return absl::OkStatus();
}

absl::Status PlayTrackerTelemetryCsv::OpenOutputs(
    const std::string& output_directory,
    const std::string& source_config_file,
    const std::string& effective_config_file) {
  std::error_code error;
  fs::create_directories(output_directory, error);
  if (error) {
    return absl::InternalError(
        absl::StrCat("could not create playtracker telemetry directory ", output_directory, ": ", error.message()));
  }
  if (!fs::is_directory(output_directory, error) || error) {
    return absl::InvalidArgumentError(
        absl::StrCat("playtracker telemetry output is not a directory: ", output_directory));
  }
  output_directory_ = fs::absolute(output_directory, error).lexically_normal().string();
  if (error) {
    return absl::InvalidArgumentError(
        absl::StrCat("could not normalize playtracker telemetry directory: ", error.message()));
  }

  const std::vector<std::pair<std::string, std::string>> artifact_names = {
      {"tracking", ".csv"},
      {"camera", ".csv"},
      {"camera_fast", ".csv"},
      {"hstream_frame_index", ".csv"},
      {"hstream_config_events", ".csv"},
      {"hstream_telemetry", ".json"},
      {"play_tracker_source", ".yaml"},
      {"play_tracker_effective", ".yaml"},
  };
  uint64_t first_generation = 0;
  fs::directory_iterator entry(output_directory_, error);
  const fs::directory_iterator end;
  while (entry != end) {
    if (error) {
      return absl::InternalError(absl::StrCat("could not inspect telemetry generations: ", error.message()));
    }
    if (!entry->is_regular_file(error) || error) {
      error.clear();
      entry.increment(error);
      continue;
    }
    const std::string filename = entry->path().filename().string();
    for (const auto& [stem, extension] : artifact_names) {
      if (filename == stem + extension) {
        first_generation = std::max<uint64_t>(first_generation, 1);
        break;
      }
      const std::string prefix = stem + "-";
      if (filename.size() <= prefix.size() + extension.size() || filename.rfind(prefix, 0) != 0 ||
          filename.compare(filename.size() - extension.size(), extension.size(), extension) != 0) {
        continue;
      }
      const std::string number = filename.substr(prefix.size(), filename.size() - prefix.size() - extension.size());
      if (number.empty() ||
          !std::all_of(number.begin(), number.end(), [](unsigned char c) { return std::isdigit(c); })) {
        continue;
      }
      try {
        const unsigned long long parsed = std::stoull(number);
        if (parsed >= std::numeric_limits<uint64_t>::max()) {
          return absl::InvalidArgumentError(absl::StrCat("telemetry generation is outside uint64 range: ", filename));
        }
        first_generation = std::max(first_generation, static_cast<uint64_t>(parsed) + 1);
      } catch (const std::exception&) {
        return absl::InvalidArgumentError(absl::StrCat("telemetry generation is outside uint64 range: ", filename));
      }
      break;
    }
    entry.increment(error);
  }
  if (error) {
    return absl::InternalError(absl::StrCat("could not inspect telemetry generations: ", error.message()));
  }

  int manifest_fd = -1;
  for (uint64_t attempt = 0; attempt < 100000; ++attempt) {
    if (first_generation > std::numeric_limits<uint64_t>::max() - attempt) {
      break;
    }
    const uint64_t generation = first_generation + attempt;
    suffix_ = generation == 0 ? "" : absl::StrCat("-", generation);
    const std::vector<std::string> generation_artifacts = {
        suffixed_name("tracking", suffix_, ".csv"),
        suffixed_name("camera", suffix_, ".csv"),
        suffixed_name("camera_fast", suffix_, ".csv"),
        suffixed_name("hstream_frame_index", suffix_, ".csv"),
        suffixed_name("hstream_config_events", suffix_, ".csv"),
        suffixed_name("play_tracker_source", suffix_, ".yaml"),
        suffixed_name("play_tracker_effective", suffix_, ".yaml"),
    };
    const bool generation_exists =
        std::any_of(generation_artifacts.begin(), generation_artifacts.end(), [&](const std::string& artifact) {
          std::error_code exists_error;
          const bool exists = fs::exists(fs::path(output_directory_) / artifact, exists_error);
          return exists || static_cast<bool>(exists_error);
        });
    if (generation_exists) {
      continue;
    }
    manifest_path_ = (fs::path(output_directory_) / suffixed_name("hstream_telemetry", suffix_, ".json")).string();
    manifest_fd = ::open(manifest_path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (manifest_fd >= 0) {
      ::close(manifest_fd);
      break;
    }
    if (errno != EEXIST) {
      return absl::InternalError(
          absl::StrCat("could not reserve playtracker telemetry manifest ", manifest_path_, ": ", strerror(errno)));
    }
  }
  if (manifest_fd < 0) {
    return absl::ResourceExhaustedError("could not reserve a unique playtracker telemetry generation");
  }

  tracking_filename_ = suffixed_name("tracking", suffix_, ".csv");
  camera_filename_ = suffixed_name("camera", suffix_, ".csv");
  camera_fast_filename_ = suffixed_name("camera_fast", suffix_, ".csv");
  frame_index_filename_ = suffixed_name("hstream_frame_index", suffix_, ".csv");
  config_events_filename_ = suffixed_name("hstream_config_events", suffix_, ".csv");
  source_config_filename_ = suffixed_name("play_tracker_source", suffix_, ".yaml");
  effective_config_filename_ = suffixed_name("play_tracker_effective", suffix_, ".yaml");

  const fs::path directory(output_directory_);
  tracking_.open(directory / tracking_filename_, std::ios::out | std::ios::trunc);
  camera_.open(directory / camera_filename_, std::ios::out | std::ios::trunc);
  camera_fast_.open(directory / camera_fast_filename_, std::ios::out | std::ios::trunc);
  frame_index_.open(directory / frame_index_filename_, std::ios::out | std::ios::trunc);
  config_events_.open(directory / config_events_filename_, std::ios::out | std::ios::trunc);
  if (!tracking_ || !camera_ || !camera_fast_ || !frame_index_ || !config_events_) {
    return absl::InternalError(absl::StrCat("could not open playtracker telemetry outputs in ", output_directory_));
  }

  const std::string source_contents = ReadTelemetryConfigArtifact(source_config_file);
  const std::string effective_contents = ReadTelemetryConfigArtifact(effective_config_file);
  if (source_contents.empty() || effective_contents.empty()) {
    return absl::InvalidArgumentError("could not read non-empty playtracker source/effective configuration");
  }
  if (!write_file(directory / source_config_filename_, source_contents) ||
      !write_file(directory / effective_config_filename_, effective_contents)) {
    return absl::InternalError("could not preserve playtracker telemetry configuration provenance");
  }
  return absl::OkStatus();
}

void PlayTrackerTelemetryCsv::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ && !writer_thread_.joinable()) {
      CloseOutputs();
      return;
    }
    stopping_ = true;
    active_ = false;
  }
  ready_.notify_all();
  if (writer_thread_.joinable()) {
    writer_thread_.join();
  }
  WriteManifest(true);
  CloseOutputs();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    stopping_ = false;
  }
}

bool PlayTrackerTelemetryCsv::active() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_;
}

bool PlayTrackerTelemetryCsv::TryEnqueue(TelemetrySample sample) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_ || stopping_) {
    return false;
  }
  if (queue_.size() >= queue_capacity_) {
    dropped_samples_.fetch_add(1, std::memory_order_acq_rel);
    return false;
  }
  const uint64_t sample_id = next_sample_id_.fetch_add(1, std::memory_order_acq_rel) + 1;
  queue_.emplace_back(QueuedSample{sample_id, std::move(sample)});
  ready_.notify_one();
  return true;
}

bool PlayTrackerTelemetryCsv::TryRecordConfigEvent(TelemetryConfigEvent event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_ || stopping_) {
    return false;
  }
  if (queue_.size() >= queue_capacity_) {
    dropped_config_events_.fetch_add(1, std::memory_order_acq_rel);
    return false;
  }
  queue_.emplace_back(QueuedConfigEvent{next_sample_id_.load(std::memory_order_acquire) + 1, std::move(event)});
  ready_.notify_one();
  return true;
}

std::string PlayTrackerTelemetryCsv::output_manifest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return manifest_path_;
}

void PlayTrackerTelemetryCsv::WriterLoop() {
  for (;;) {
    WorkItem item;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_) {
          break;
        }
        continue;
      }
      item = std::move(queue_.front());
      queue_.pop_front();
    }
    if (const auto* sample = std::get_if<QueuedSample>(&item)) {
      WriteSample(*sample);
    } else {
      WriteConfigEvent(std::get<QueuedConfigEvent>(item));
    }
    if (!tracking_ || !camera_ || !camera_fast_ || !frame_index_ || !config_events_) {
      writer_failed_ = true;
    }
  }
  tracking_.flush();
  camera_.flush();
  camera_fast_.flush();
  frame_index_.flush();
  config_events_.flush();
}

void PlayTrackerTelemetryCsv::WriteSample(const QueuedSample& queued) {
  const TelemetrySample& sample = queued.sample;
  for (const TelemetryTrack& track : sample.tracks) {
    // Exact current HM TrackingDataFrame order (headerless):
    // Frame,ID,BBox_X,BBox_Y,BBox_W,BBox_H,Scores,Labels,Visibility,
    // JerseyInfo,ActionLabel,ActionScore,ActionIndex.
    tracking_ << queued.sample_id << ',' << track.tracking_id << ',' << std::setprecision(9) << track.left << ','
              << track.top << ',' << track.width << ',' << track.height << ',' << track.score << ',' << track.class_id
              << ",-1,{},,0,-1\n";
  }
  if (!sample.policy_boxes.empty()) {
    write_camera_row(camera_, queued.sample_id, sample.policy_boxes.back());
    write_camera_row(camera_fast_, queued.sample_id, sample.policy_boxes.front());
  }

  frame_index_ << queued.sample_id << ',' << sample.source_id << ',' << sample.source_frame << ',';
  write_optional(frame_index_, sample.decoded_source_id);
  frame_index_ << ',';
  write_optional(frame_index_, sample.decoded_sequence);
  frame_index_ << ',';
  write_optional(frame_index_, sample.pts_ns);
  frame_index_ << ',';
  write_optional(frame_index_, sample.ntp_ns);
  frame_index_ << ',' << sample.seek_epoch << ',' << sample.width << ',' << sample.height << ',' << sample.tracks.size()
               << ',' << (!sample.policy_boxes.empty() ? 1 : 0) << '\n';
}

void PlayTrackerTelemetryCsv::WriteConfigEvent(const QueuedConfigEvent& queued) {
  ++config_event_sequence_;
  std::string artifact;
  if (!queued.event.artifact_contents.empty()) {
    ++config_artifact_sequence_;
    std::string stem = queued.event.artifact_stem.empty() ? "play_tracker_event" : queued.event.artifact_stem;
    for (char& character : stem) {
      if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '-' || character == '_')) {
        character = '_';
      }
    }
    artifact = absl::StrCat(stem, suffix_, "-", config_artifact_sequence_, ".yaml");
    if (!write_file(fs::path(output_directory_) / artifact, queued.event.artifact_contents)) {
      writer_failed_ = true;
      artifact.clear();
    }
  }
  config_events_ << config_event_sequence_ << ',' << queued.sample_boundary << ',' << csv_string(queued.event.kind)
                 << ',' << csv_string(queued.event.key) << ',' << csv_string(queued.event.value) << ','
                 << csv_string(artifact) << '\n';
}

void PlayTrackerTelemetryCsv::WriteManifest(bool complete) {
  if (manifest_path_.empty()) {
    return;
  }
  std::ofstream manifest(manifest_path_, std::ios::out | std::ios::trunc);
  if (!manifest) {
    writer_failed_ = true;
    return;
  }
  manifest << "{\n"
           << "  \"schema\": \"hstream-playtracker-telemetry-v1\",\n"
           << "  \"started_utc\": " << json_string(started_utc_) << ",\n"
           << "  \"output_directory\": " << json_string(output_directory_) << ",\n"
           << "  \"completed\": " << (complete ? "true" : "false") << ",\n"
           << "  \"writer_failed\": " << (writer_failed_ ? "true" : "false") << ",\n"
           << "  \"samples_written_or_queued\": " << next_sample_id_.load(std::memory_order_acquire) << ",\n"
           << "  \"dropped_samples\": " << dropped_samples_.load(std::memory_order_acquire) << ",\n"
           << "  \"dropped_config_events\": " << dropped_config_events_.load(std::memory_order_acquire) << ",\n"
           << "  \"performance_contract\": \"metadata-only; no video-frame mapping or CPU pixel conversion\",\n"
           << "  \"hm_compatibility\": {\n"
           << "    \"tracking_csv\": {\"file\": " << json_string(tracking_filename_)
           << ", \"header\": false, \"columns\": "
              "[\"Frame\",\"ID\",\"BBox_X\",\"BBox_Y\",\"BBox_W\",\"BBox_H\",\"Scores\","
              "\"Labels\",\"Visibility\",\"JerseyInfo\",\"ActionLabel\",\"ActionScore\",\"ActionIndex\"]},\n"
           << "    \"camera_csv\": {\"file\": " << json_string(camera_filename_)
           << ", \"header\": false, \"columns\": [\"Frame\",\"BBox_X\",\"BBox_Y\",\"BBox_W\",\"BBox_H\"], "
              "\"policy_role\": \"follower/program\"},\n"
           << "    \"camera_fast_csv\": {\"file\": " << json_string(camera_fast_filename_)
           << ", \"header\": false, \"columns\": [\"Frame\",\"BBox_X\",\"BBox_Y\",\"BBox_W\",\"BBox_H\"], "
              "\"policy_role\": \"fast\"},\n"
           << "    \"frame_identity\": \"Frame is a monotonically assigned export Sample shared by all three HM CSVs; "
              "native source/frame/PTS identity is in the frame-index sidecar\"\n"
           << "  },\n"
           << "  \"sidecars\": {\"frame_index\": " << json_string(frame_index_filename_)
           << ", \"config_events\": " << json_string(config_events_filename_) << "},\n"
           << "  \"config_provenance\": {\n"
           << "    \"source_path\": " << json_string(source_config_path_) << ",\n"
           << "    \"effective_path\": " << json_string(effective_config_path_) << ",\n"
           << "    \"source_artifact\": " << json_string(source_config_filename_) << ",\n"
           << "    \"effective_artifact\": " << json_string(effective_config_filename_) << "\n"
           << "  }\n"
           << "}\n";
  if (!manifest) {
    writer_failed_ = true;
  }
}

void PlayTrackerTelemetryCsv::CloseOutputs() {
  tracking_.close();
  camera_.close();
  camera_fast_.close();
  frame_index_.close();
  config_events_.close();
}

} // namespace hm::playtracker
