#include "hstream/src/gst-plugins/gst-videoprep/playtracker/PlayTrackerTelemetryCsv.h"

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
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

bool write_all(int fd, const std::string& contents) {
  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t written = ::write(fd, contents.data() + offset, contents.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (written == 0) {
      return false;
    }
    offset += static_cast<size_t>(written);
  }
  return true;
}

bool replace_file_contents(int fd, const std::string& contents) {
  if (::ftruncate(fd, 0) != 0 || ::lseek(fd, 0, SEEK_SET) < 0) {
    return false;
  }
  return write_all(fd, contents);
}

bool open_reserved_stream(int fd, std::ofstream* output) {
  if (!output) {
    return false;
  }
  output->open(absl::StrCat("/proc/self/fd/", fd), std::ios::out | std::ios::binary);
  return output->good();
}

bool unlink_owned_name(int directory_fd, const std::string& filename, uint64_t device, uint64_t inode) {
  struct stat info{};
  return ::fstatat(directory_fd, filename.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(info.st_mode) &&
      static_cast<uint64_t>(info.st_dev) == device && static_cast<uint64_t>(info.st_ino) == inode &&
      ::unlinkat(directory_fd, filename.c_str(), 0) == 0;
}

const char* outcome_name(TelemetryRunOutcome outcome) {
  switch (outcome) {
    case TelemetryRunOutcome::kIncomplete:
      return "incomplete";
    case TelemetryRunOutcome::kEndOfStream:
      return "end-of-stream";
    case TelemetryRunOutcome::kIntentionalStop:
      return "intentional-stop";
    case TelemetryRunOutcome::kFailed:
      return "failed";
  }
  return "incomplete";
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

  ResetOutputPaths();
  queue_capacity_ = queue_capacity;
  frame_id_high_watermark_.store(0, std::memory_order_release);
  attempted_samples_.store(0, std::memory_order_release);
  discontinuity_gaps_.store(0, std::memory_order_release);
  config_event_discontinuity_gaps_.store(0, std::memory_order_release);
  dropped_samples_.store(0, std::memory_order_release);
  dropped_config_events_.store(0, std::memory_order_release);
  samples_persisted_.store(0, std::memory_order_release);
  training_samples_persisted_.store(0, std::memory_order_release);
  config_events_attempted_.store(0, std::memory_order_release);
  config_events_persisted_.store(0, std::memory_order_release);
  fail_next_config_artifact_write_for_testing_.store(false, std::memory_order_release);
  config_event_sequence_ = 0;
  config_artifact_sequence_ = 0;
  writer_failed_ = false;
  writer_drained_ = false;
  completed_ = false;
  eligible_for_training_ = false;
  run_outcome_ = TelemetryRunOutcome::kIncomplete;
  stopping_ = false;
  started_utc_ = utc_now();
  source_config_path_ = source_config_file;
  effective_config_path_ = effective_config_file;
  const absl::Status open_status = OpenOutputs(output_directory, source_config_file, effective_config_file);
  if (!open_status.ok()) {
    RemoveIncompleteOutputs();
    CloseOutputs();
    return open_status;
  }

  frame_index_ << "Sample,SourceID,SourceFrame,DecodedSourceID,DecodedSequence,PTS_NS,NTP_NS,SeekEpoch,Width,Height,"
                  "TrackCount,HasCamera\n";
  config_events_ << "Event,SampleBoundary,Kind,Key,Value,Artifact\n";
  config_events_ << "0,1,base-config,config-file," << csv_string(source_config_file) << ','
                 << csv_string(source_config_filename_) << '\n';
  if (!tracking_ || !camera_ || !camera_fast_ || !frame_index_ || !config_events_) {
    RemoveIncompleteOutputs();
    CloseOutputs();
    return absl::InternalError("could not initialize playtracker telemetry CSV headers");
  }

  if (!WriteManifest()) {
    RemoveIncompleteOutputs();
    CloseOutputs();
    return absl::InternalError("could not write initial playtracker telemetry manifest");
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = true;
  }
  try {
    writer_thread_ = std::thread(&PlayTrackerTelemetryCsv::WriterLoop, this);
  } catch (const std::exception& error) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_ = false;
      queue_.clear();
    }
    RemoveIncompleteOutputs();
    CloseOutputs();
    return absl::ResourceExhaustedError(absl::StrCat("could not start telemetry writer thread: ", error.what()));
  }
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

  const std::string source_contents = ReadTelemetryConfigArtifact(source_config_file);
  const std::string effective_contents = ReadTelemetryConfigArtifact(effective_config_file);
  if (source_contents.empty() || effective_contents.empty()) {
    return absl::InvalidArgumentError("could not read non-empty playtracker source/effective configuration");
  }

  output_directory_fd_ = ::open(output_directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (output_directory_fd_ < 0) {
    return absl::InternalError(absl::StrCat("could not open playtracker telemetry directory: ", strerror(errno)));
  }
  directory_lock_fd_ =
      ::openat(output_directory_fd_, ".hstream-telemetry.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (directory_lock_fd_ < 0) {
    return absl::InternalError(absl::StrCat("could not open playtracker telemetry directory lock: ", strerror(errno)));
  }
  if (::flock(directory_lock_fd_, LOCK_EX) != 0) {
    return absl::InternalError(absl::StrCat("could not lock playtracker telemetry directory: ", strerror(errno)));
  }
  const auto unlock_directory = [this]() { ::flock(directory_lock_fd_, LOCK_UN); };

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
      unlock_directory();
      return absl::InternalError(absl::StrCat("could not inspect telemetry generations: ", error.message()));
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
          unlock_directory();
          return absl::InvalidArgumentError(absl::StrCat("telemetry generation is outside uint64 range: ", filename));
        }
        first_generation = std::max(first_generation, static_cast<uint64_t>(parsed) + 1);
      } catch (const std::exception&) {
        unlock_directory();
        return absl::InvalidArgumentError(absl::StrCat("telemetry generation is outside uint64 range: ", filename));
      }
      break;
    }
    entry.increment(error);
  }
  if (error) {
    unlock_directory();
    return absl::InternalError(absl::StrCat("could not inspect telemetry generations: ", error.message()));
  }

  std::array<int, 8> artifact_fds;
  artifact_fds.fill(-1);
  bool generation_reserved = false;
  for (uint64_t attempt = 0; attempt < 100000; ++attempt) {
    if (first_generation > std::numeric_limits<uint64_t>::max() - attempt) {
      break;
    }
    const uint64_t generation = first_generation + attempt;
    suffix_ = generation == 0 ? "" : absl::StrCat("-", generation);
    tracking_filename_ = suffixed_name("tracking", suffix_, ".csv");
    camera_filename_ = suffixed_name("camera", suffix_, ".csv");
    camera_fast_filename_ = suffixed_name("camera_fast", suffix_, ".csv");
    frame_index_filename_ = suffixed_name("hstream_frame_index", suffix_, ".csv");
    config_events_filename_ = suffixed_name("hstream_config_events", suffix_, ".csv");
    source_config_filename_ = suffixed_name("play_tracker_source", suffix_, ".yaml");
    effective_config_filename_ = suffixed_name("play_tracker_effective", suffix_, ".yaml");
    struct ArtifactReservation {
      std::string filename;
      std::string published_filename;
      bool training_input;
    };
    const std::array<ArtifactReservation, 8> generation_artifacts = {{
        {absl::StrCat(".", tracking_filename_, ".partial"), tracking_filename_, true},
        {absl::StrCat(".", camera_filename_, ".partial"), camera_filename_, true},
        {absl::StrCat(".", camera_fast_filename_, ".partial"), camera_fast_filename_, true},
        {frame_index_filename_, {}, false},
        {config_events_filename_, {}, false},
        {suffixed_name("hstream_telemetry", suffix_, ".json"), {}, false},
        {source_config_filename_, {}, false},
        {effective_config_filename_, {}, false},
    }};
    const size_t owned_start = owned_artifacts_.size();
    int reservation_error = 0;
    for (size_t index = 0; index < generation_artifacts.size(); ++index) {
      artifact_fds[index] = ::openat(
          output_directory_fd_,
          generation_artifacts[index].filename.c_str(),
          O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
          0600);
      if (artifact_fds[index] < 0) {
        reservation_error = errno;
        break;
      }
      struct stat info{};
      if (::fstat(artifact_fds[index], &info) != 0) {
        reservation_error = errno;
        break;
      }
      owned_artifacts_.push_back(
          {generation_artifacts[index].filename,
           generation_artifacts[index].published_filename,
           static_cast<uint64_t>(info.st_dev),
           static_cast<uint64_t>(info.st_ino),
           generation_artifacts[index].training_input ? artifact_fds[index] : -1,
           generation_artifacts[index].training_input});
    }
    if (reservation_error == 0) {
      manifest_path_ = (fs::path(output_directory_) / generation_artifacts[5].filename).string();
      generation_reserved = true;
      break;
    }
    for (size_t index = 0; index < artifact_fds.size(); ++index) {
      if (artifact_fds[index] >= 0) {
        ::close(artifact_fds[index]);
        artifact_fds[index] = -1;
      }
    }
    for (size_t index = owned_start; index < owned_artifacts_.size(); ++index) {
      const OwnedArtifact& artifact = owned_artifacts_[index];
      unlink_owned_name(output_directory_fd_, artifact.filename, artifact.device, artifact.inode);
    }
    owned_artifacts_.resize(owned_start);
    if (reservation_error != EEXIST && reservation_error != ELOOP) {
      unlock_directory();
      return absl::InternalError(
          absl::StrCat("could not reserve playtracker telemetry generation: ", strerror(reservation_error)));
    }
  }
  if (!generation_reserved) {
    unlock_directory();
    return absl::ResourceExhaustedError("could not reserve a unique playtracker telemetry generation");
  }

  const bool provenance_written =
      write_all(artifact_fds[6], source_contents) && write_all(artifact_fds[7], effective_contents);
  const bool streams_opened = open_reserved_stream(artifact_fds[0], &tracking_) &&
      open_reserved_stream(artifact_fds[1], &camera_) && open_reserved_stream(artifact_fds[2], &camera_fast_) &&
      open_reserved_stream(artifact_fds[3], &frame_index_) && open_reserved_stream(artifact_fds[4], &config_events_);
  manifest_fd_ = artifact_fds[5];
  artifact_fds[5] = -1;
  // Keep stable descriptors for the hidden training files so publication can
  // link the exact reserved inodes rather than trusting mutable pathnames.
  artifact_fds[0] = -1;
  artifact_fds[1] = -1;
  artifact_fds[2] = -1;
  for (int& fd : artifact_fds) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
  unlock_directory();
  if (!provenance_written || !streams_opened) {
    return absl::InternalError(
        absl::StrCat("could not initialize playtracker telemetry outputs in ", output_directory_));
  }
  return absl::OkStatus();
}

void PlayTrackerTelemetryCsv::MarkRunOutcome(TelemetryRunOutcome outcome) {
  if (outcome == TelemetryRunOutcome::kIncomplete) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_ && !stopping_) {
    // A fatal pipeline result may arrive on the bus after this element has
    // already handled EOS. Failure is terminal and must downgrade that local
    // EOS observation before Stop() decides publication eligibility.
    if (outcome == TelemetryRunOutcome::kFailed || run_outcome_ != TelemetryRunOutcome::kFailed) {
      run_outcome_ = outcome;
    }
  }
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
  writer_drained_ = true;
  if (!tracking_ || !camera_ || !camera_fast_ || !frame_index_ || !VerifyOwnedArtifacts()) {
    writer_failed_ = true;
  }
  const bool successful_outcome =
      run_outcome_ == TelemetryRunOutcome::kEndOfStream || run_outcome_ == TelemetryRunOutcome::kIntentionalStop;
  completed_ = successful_outcome && !writer_failed_ && training_samples_persisted_.load(std::memory_order_acquire) > 0;
  eligible_for_training_ = completed_;
  // Finalize every fallible audit write before making tracking*.csv visible.
  // HM treats that filename as the generation's discovery/commit marker.
  if (!WriteManifest()) {
    writer_failed_ = true;
    completed_ = false;
    eligible_for_training_ = false;
  } else if (completed_ && !PublishTrainingArtifacts()) {
    writer_failed_ = true;
    completed_ = false;
    eligible_for_training_ = false;
    // Best-effort correction: publication failed before the tracking commit
    // point, so HM cannot select this generation even if this rewrite fails.
    WriteManifest();
  }
  if (!eligible_for_training_) {
    RemoveOwnedArtifacts(/*training_only=*/true);
  }
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
  return TryEnqueueLocked(std::move(sample));
}

bool PlayTrackerTelemetryCsv::TryEnqueueLocked(TelemetrySample sample) {
  if (!active_ || stopping_) {
    return false;
  }
  // Frame identity follows attempted input samples, not successful writes.
  // Reserving before admission leaves a detectable numeric gap on overflow.
  const uint64_t sample_id = frame_id_high_watermark_.fetch_add(1, std::memory_order_acq_rel) + 1;
  attempted_samples_.fetch_add(1, std::memory_order_acq_rel);
  if (queue_.size() >= queue_capacity_) {
    dropped_samples_.fetch_add(1, std::memory_order_acq_rel);
    return false;
  }
  queue_.emplace_back(QueuedSample{sample_id, std::move(sample)});
  ready_.notify_one();
  return true;
}

bool PlayTrackerTelemetryCsv::TryRecordConfigEvent(TelemetryConfigEvent event) {
  std::lock_guard<std::mutex> lock(mutex_);
  return TryRecordConfigEventLocked(std::move(event));
}

bool PlayTrackerTelemetryCsv::TryRecordConfigEventLocked(TelemetryConfigEvent event) {
  if (!active_ || stopping_) {
    return false;
  }
  // A live policy mutation has already been applied by the caller. Reserve a
  // boundary unconditionally so queue admission and later sidecar/artifact I/O
  // failures can never leave pre/post-policy samples numerically adjacent.
  config_events_attempted_.fetch_add(1, std::memory_order_acq_rel);
  frame_id_high_watermark_.fetch_add(1, std::memory_order_acq_rel);
  discontinuity_gaps_.fetch_add(1, std::memory_order_acq_rel);
  config_event_discontinuity_gaps_.fetch_add(1, std::memory_order_acq_rel);
  if (queue_.size() >= queue_capacity_) {
    dropped_config_events_.fetch_add(1, std::memory_order_acq_rel);
    return false;
  }
  queue_.emplace_back(
      QueuedConfigEvent{frame_id_high_watermark_.load(std::memory_order_acquire) + 1, std::move(event)});
  ready_.notify_one();
  return true;
}

bool PlayTrackerTelemetryCsv::TryRecordDiscontinuity(TelemetryConfigEvent event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_ || stopping_) {
    return false;
  }
  config_events_attempted_.fetch_add(1, std::memory_order_acq_rel);
  // No row may occupy this ID. The next accepted sample therefore cannot be
  // interpreted as adjacent to the pre-seek timeline.
  frame_id_high_watermark_.fetch_add(1, std::memory_order_acq_rel);
  discontinuity_gaps_.fetch_add(1, std::memory_order_acq_rel);
  if (queue_.size() >= queue_capacity_) {
    dropped_config_events_.fetch_add(1, std::memory_order_acq_rel);
    return false;
  }
  queue_.emplace_back(
      QueuedConfigEvent{frame_id_high_watermark_.load(std::memory_order_acquire) + 1, std::move(event)});
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
    if (!tracking_ || !camera_ || !camera_fast_ || !frame_index_) {
      writer_failed_ = true;
    }
  }
  tracking_.flush();
  camera_.flush();
  camera_fast_.flush();
  frame_index_.flush();
  config_events_.flush();
  if (!config_events_) {
    config_events_persisted_.store(0, std::memory_order_release);
  }
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
  if (tracking_ && camera_ && camera_fast_ && frame_index_) {
    samples_persisted_.fetch_add(1, std::memory_order_acq_rel);
    if (!sample.tracks.empty() && !sample.policy_boxes.empty()) {
      training_samples_persisted_.fetch_add(1, std::memory_order_acq_rel);
    }
  }
}

bool PlayTrackerTelemetryCsv::WriteConfigEvent(const QueuedConfigEvent& queued) {
  ++config_event_sequence_;
  std::string artifact;
  const bool artifact_required = !queued.event.artifact_stem.empty() || !queued.event.artifact_contents.empty();
  if (artifact_required) {
    if (queued.event.artifact_contents.empty()) {
      return false;
    }
    std::string stem = queued.event.artifact_stem.empty() ? "play_tracker_event" : queued.event.artifact_stem;
    for (char& character : stem) {
      if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '-' || character == '_')) {
        character = '_';
      }
    }
    if (!WriteExclusiveConfigArtifact(stem, queued.event.artifact_contents, &artifact)) {
      return false;
    }
  }
  config_events_ << config_event_sequence_ << ',' << queued.sample_boundary << ',' << csv_string(queued.event.kind)
                 << ',' << csv_string(queued.event.key) << ',' << csv_string(queued.event.value) << ','
                 << csv_string(artifact) << '\n';
  if (!config_events_) {
    return false;
  }
  config_events_persisted_.fetch_add(1, std::memory_order_acq_rel);
  return true;
}

bool PlayTrackerTelemetryCsv::WriteExclusiveConfigArtifact(
    const std::string& stem,
    const std::string& contents,
    std::string* filename) {
  if (!filename || output_directory_fd_ < 0 || directory_lock_fd_ < 0) {
    return false;
  }
  if (::flock(directory_lock_fd_, LOCK_EX) != 0) {
    return false;
  }
  if (fail_next_config_artifact_write_for_testing_.exchange(false, std::memory_order_acq_rel)) {
    ::flock(directory_lock_fd_, LOCK_UN);
    return false;
  }
  bool written = false;
  for (size_t attempt = 0; attempt < 100000; ++attempt) {
    ++config_artifact_sequence_;
    const std::string candidate = absl::StrCat(stem, suffix_, "-", config_artifact_sequence_, ".yaml");
    const int fd =
        ::openat(output_directory_fd_, candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
      if (errno == EEXIST || errno == ELOOP) {
        continue;
      }
      break;
    }
    struct stat info{};
    const bool have_identity = ::fstat(fd, &info) == 0;
    written = have_identity && write_all(fd, contents);
    ::close(fd);
    if (!written) {
      if (have_identity) {
        unlink_owned_name(
            output_directory_fd_, candidate, static_cast<uint64_t>(info.st_dev), static_cast<uint64_t>(info.st_ino));
      }
      break;
    }
    owned_artifacts_.push_back(
        {candidate, {}, static_cast<uint64_t>(info.st_dev), static_cast<uint64_t>(info.st_ino), -1, false});
    *filename = candidate;
    break;
  }
  ::flock(directory_lock_fd_, LOCK_UN);
  return written;
}

bool PlayTrackerTelemetryCsv::PublishTrainingArtifacts() {
  if (output_directory_fd_ < 0 || directory_lock_fd_ < 0 || ::flock(directory_lock_fd_, LOCK_EX) != 0) {
    return false;
  }

  std::vector<size_t> publication_order;
  publication_order.reserve(3);
  // Publish tracking last because HM discovers a generation from tracking*.csv.
  // Its appearance therefore means both camera inputs are already present.
  const std::array<std::string, 3> filenames = {camera_filename_, camera_fast_filename_, tracking_filename_};
  for (const std::string& filename : filenames) {
    const auto artifact =
        std::find_if(owned_artifacts_.begin(), owned_artifacts_.end(), [&filename](const OwnedArtifact& candidate) {
          return candidate.training_input && candidate.published_filename == filename;
        });
    if (artifact == owned_artifacts_.end()) {
      ::flock(directory_lock_fd_, LOCK_UN);
      return false;
    }
    publication_order.push_back(static_cast<size_t>(std::distance(owned_artifacts_.begin(), artifact)));
  }

  bool success = true;
  for (const size_t index : publication_order) {
    const OwnedArtifact& artifact = owned_artifacts_[index];
    struct stat staging_info{};
    struct stat reserved_info{};
    if (artifact.reservation_fd < 0 || ::fstat(artifact.reservation_fd, &reserved_info) != 0 ||
        !S_ISREG(reserved_info.st_mode) || static_cast<uint64_t>(reserved_info.st_dev) != artifact.device ||
        static_cast<uint64_t>(reserved_info.st_ino) != artifact.inode ||
        ::fstatat(output_directory_fd_, artifact.filename.c_str(), &staging_info, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(staging_info.st_mode) || static_cast<uint64_t>(staging_info.st_dev) != artifact.device ||
        static_cast<uint64_t>(staging_info.st_ino) != artifact.inode) {
      success = false;
      break;
    }
    const std::string reserved_path = absl::StrCat("/proc/self/fd/", artifact.reservation_fd);
    if (::linkat(
            AT_FDCWD,
            reserved_path.c_str(),
            output_directory_fd_,
            artifact.published_filename.c_str(),
            AT_SYMLINK_FOLLOW) != 0) {
      success = false;
      break;
    }
  }

  if (!success) {
    // Roll back only links that still name one of our reserved inodes. A
    // competing file or symlink is never followed or removed.
    for (const size_t index : publication_order) {
      const OwnedArtifact& artifact = owned_artifacts_[index];
      unlink_owned_name(output_directory_fd_, artifact.published_filename, artifact.device, artifact.inode);
    }
    ::flock(directory_lock_fd_, LOCK_UN);
    return false;
  }

  // The tracking link above was the commit point. Hidden-link cleanup is
  // deliberately best-effort: no failure after that commit can revoke an
  // otherwise valid generation or require unsafe rollback.
  for (const size_t index : publication_order) {
    const OwnedArtifact& artifact = owned_artifacts_[index];
    unlink_owned_name(output_directory_fd_, artifact.filename, artifact.device, artifact.inode);
    owned_artifacts_[index].filename = owned_artifacts_[index].published_filename;
  }
  ::flock(directory_lock_fd_, LOCK_UN);
  return true;
}

bool PlayTrackerTelemetryCsv::VerifyOwnedArtifacts() const {
  if (output_directory_fd_ < 0 || directory_lock_fd_ < 0 || ::flock(directory_lock_fd_, LOCK_EX) != 0) {
    return false;
  }
  bool valid = true;
  for (const OwnedArtifact& artifact : owned_artifacts_) {
    struct stat info{};
    if (::fstatat(output_directory_fd_, artifact.filename.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(info.st_mode) || static_cast<uint64_t>(info.st_dev) != artifact.device ||
        static_cast<uint64_t>(info.st_ino) != artifact.inode) {
      valid = false;
      break;
    }
  }
  ::flock(directory_lock_fd_, LOCK_UN);
  return valid;
}

void PlayTrackerTelemetryCsv::RemoveOwnedArtifacts(bool training_only) {
  if (output_directory_fd_ < 0 || directory_lock_fd_ < 0 || ::flock(directory_lock_fd_, LOCK_EX) != 0) {
    return;
  }
  for (const OwnedArtifact& artifact : owned_artifacts_) {
    if (training_only && !artifact.training_input) {
      continue;
    }
    unlink_owned_name(output_directory_fd_, artifact.filename, artifact.device, artifact.inode);
  }
  ::flock(directory_lock_fd_, LOCK_UN);
}

bool PlayTrackerTelemetryCsv::WriteManifest() {
  if (manifest_fd_ < 0) {
    return false;
  }
  const uint64_t config_events_attempted = config_events_attempted_.load(std::memory_order_acquire);
  const uint64_t config_events_persisted = config_events_persisted_.load(std::memory_order_acquire);
  std::ostringstream manifest;
  manifest << "{\n"
           << "  \"schema\": \"hstream-playtracker-telemetry-v1\",\n"
           << "  \"started_utc\": " << json_string(started_utc_) << ",\n"
           << "  \"output_directory\": " << json_string(output_directory_) << ",\n"
           << "  \"writer_drained\": " << (writer_drained_ ? "true" : "false") << ",\n"
           << "  \"run_outcome\": " << json_string(outcome_name(run_outcome_)) << ",\n"
           << "  \"completed\": " << (completed_ ? "true" : "false") << ",\n"
           << "  \"eligible_for_training\": " << (eligible_for_training_ ? "true" : "false") << ",\n"
           << "  \"writer_failed\": " << (writer_failed_ ? "true" : "false") << ",\n"
           << "  \"frame_id_high_watermark\": " << frame_id_high_watermark_.load(std::memory_order_acquire) << ",\n"
           << "  \"samples_attempted\": " << attempted_samples_.load(std::memory_order_acquire) << ",\n"
           << "  \"samples_enqueued\": "
           << attempted_samples_.load(std::memory_order_acquire) - dropped_samples_.load(std::memory_order_acquire)
           << ",\n"
           << "  \"discontinuity_gaps\": " << discontinuity_gaps_.load(std::memory_order_acquire) << ",\n"
           << "  \"config_event_discontinuity_gaps\": "
           << config_event_discontinuity_gaps_.load(std::memory_order_acquire) << ",\n"
           << "  \"dropped_samples\": " << dropped_samples_.load(std::memory_order_acquire) << ",\n"
           << "  \"dropped_config_events\": " << dropped_config_events_.load(std::memory_order_acquire) << ",\n"
           << "  \"samples_persisted\": " << samples_persisted_.load(std::memory_order_acquire) << ",\n"
           << "  \"training_samples_persisted\": " << training_samples_persisted_.load(std::memory_order_acquire)
           << ",\n"
           << "  \"config_events_attempted\": " << config_events_attempted << ",\n"
           << "  \"config_events_persisted\": " << config_events_persisted << ",\n"
           << "  \"config_events_lost\": " << config_events_attempted - config_events_persisted << ",\n"
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
           << "    \"frame_identity\": \"Frame is a monotonic attempted-sample ID shared by all three HM CSVs; "
              "queue drops, seeks, live policy mutations, and missing schema rows leave gaps that split training "
              "windows; native source/frame/PTS identity is in the frame-index sidecar\"\n"
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
  return replace_file_contents(manifest_fd_, manifest.str());
}

void PlayTrackerTelemetryCsv::CloseOutputs() {
  tracking_.close();
  camera_.close();
  camera_fast_.close();
  frame_index_.close();
  config_events_.close();
  for (OwnedArtifact& artifact : owned_artifacts_) {
    if (artifact.reservation_fd >= 0) {
      ::close(artifact.reservation_fd);
      artifact.reservation_fd = -1;
    }
  }
  if (manifest_fd_ >= 0) {
    ::close(manifest_fd_);
    manifest_fd_ = -1;
  }
  if (directory_lock_fd_ >= 0) {
    ::close(directory_lock_fd_);
    directory_lock_fd_ = -1;
  }
  if (output_directory_fd_ >= 0) {
    ::close(output_directory_fd_);
    output_directory_fd_ = -1;
  }
}

void PlayTrackerTelemetryCsv::RemoveIncompleteOutputs() {
  RemoveOwnedArtifacts(/*training_only=*/false);
}

void PlayTrackerTelemetryCsv::ResetOutputPaths() {
  output_directory_.clear();
  suffix_.clear();
  manifest_path_.clear();
  tracking_filename_.clear();
  camera_filename_.clear();
  camera_fast_filename_.clear();
  frame_index_filename_.clear();
  config_events_filename_.clear();
  source_config_filename_.clear();
  effective_config_filename_.clear();
  owned_artifacts_.clear();
}

} // namespace hm::playtracker
