#include "hstream/src/libs/playtracker_replay/ReplaySession.h"

#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "hockeymon/csrc/play_tracker/PlayTrackerSnapshot.h"
#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerCtx.h"
#include "yaml-cpp/yaml.h"

namespace hm::playtracker_replay {
namespace {
namespace fs = std::filesystem;
using Tracker = DsPlayTrackerCtx::PlayTracker;
using Snapshot = hm::play_tracker::PlayTrackerSnapshot;
constexpr size_t kMaxLine = 32 * 1024 * 1024;
constexpr size_t kMaxArtifact = 16 * 1024 * 1024;
constexpr size_t kMaxRows = 10000000;
constexpr size_t kMaxRangeSamples = 18000;
constexpr size_t kMaxRangeTracks = 1000000;

struct ReplayError : std::runtime_error {
  using std::runtime_error::runtime_error;
};
void require(bool condition, const std::string& message) {
  if (!condition)
    throw ReplayError(message);
}
void check_cancel(const std::atomic<bool>* cancelled) {
  if (cancelled && cancelled->load(std::memory_order_relaxed))
    throw ReplayError("Replay cancelled");
}

// getline's allocation is unbounded on malformed input. This reader caps each
// physical line before appending and checks cancellation within long records.
class Lines {
 public:
  Lines(const std::string& path, const std::atomic<bool>* cancelled)
      : input_(path), path_(path), cancelled_(cancelled) {
    require(input_.is_open(), "Cannot open recording artifact: " + path);
  }
  bool next(std::string* output) {
    output->clear();
    check_cancel(cancelled_);
    char c;
    while (input_.get(c)) {
      if (c == '\n')
        break;
      require(output->size() < kMaxLine, "Recording line exceeds 32 MiB: " + path_);
      output->push_back(c);
      if (output->size() % 65536 == 0)
        check_cancel(cancelled_);
    }
    require(!input_.bad(), "Cannot read recording artifact: " + path_);
    if (output->empty() && input_.eof())
      return false;
    if (!output->empty() && output->back() == '\r')
      output->pop_back();
    require(++rows_ <= kMaxRows, "Recording exceeds replay row limit");
    return true;
  }

 private:
  std::ifstream input_;
  std::string path_;
  const std::atomic<bool>* cancelled_;
  size_t rows_{0};
};
std::string read_artifact(const std::string& path) {
  require(fs::is_regular_file(path), "Missing original recording artifact: " + path);
  require(fs::file_size(path) <= kMaxArtifact, "Recording artifact exceeds 16 MiB: " + path);
  std::ifstream input(path, std::ios::binary);
  require(input.is_open(), "Cannot open recording artifact: " + path);
  const size_t capacity = static_cast<size_t>(fs::file_size(path)) + 1;
  std::string contents(std::min(capacity, kMaxArtifact + 1), '\0');
  input.read(contents.data(), contents.size());
  const size_t count = static_cast<size_t>(input.gcount());
  require(!input.bad() && count < contents.size(), "Recording artifact changed or exceeded its size limit: " + path);
  contents.resize(count);
  return contents;
}
std::string artifact_path(const fs::path& directory, const YAML::Node& filename) {
  require(filename && filename.IsScalar(), "Recording manifest is missing a required artifact filename");
  const fs::path name(filename.as<std::string>());
  require(
      !name.empty() && name == name.filename() && name != "." && name != "..",
      "Recording artifact must be a filename in the manifest directory");
  return (directory / name).string();
}
uint64_t u64(const std::string& value) {
  require(!value.empty() && value.front() != '-', "Missing/negative recording sample or timestamp");
  size_t end = 0;
  const uint64_t result = std::stoull(value, &end);
  require(end == value.size(), "Invalid integer in recording");
  return result;
}
float finite_float(const std::string& value) {
  size_t end = 0;
  const float result = std::stof(value, &end);
  require(end == value.size() && std::isfinite(result), "Invalid non-finite recording coordinate");
  return result;
}
std::vector<std::string> csv(const std::string& line) {
  std::vector<std::string> fields;
  std::string value;
  bool quoted = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '"') {
      if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
        value += '"';
        ++i;
      } else {
        quoted = !quoted;
      }
    } else if (c == ',' && !quoted) {
      require(fields.size() < 32, "Recording CSV has excessive columns");
      fields.push_back(std::move(value));
      value.clear();
    } else {
      value += c;
    }
  }
  require(!quoted, "Unclosed quote in recording CSV");
  fields.push_back(std::move(value));
  return fields;
}
void validate_box(const hm::BBox& b) {
  require(
      std::isfinite(b.left) && std::isfinite(b.top) && std::isfinite(b.right) && std::isfinite(b.bottom) &&
          b.right > b.left && b.bottom > b.top,
      "Recording contains a non-finite or empty rectangle");
}
hm::BBox tlwh(const std::vector<std::string>& row, size_t offset) {
  const float x = finite_float(row.at(offset)), y = finite_float(row.at(offset + 1));
  const hm::BBox box(x, y, x + finite_float(row.at(offset + 2)), y + finite_float(row.at(offset + 3)));
  validate_box(box);
  return box;
}
hm::BBox box_node(const YAML::Node& node, size_t offset = 0) {
  require(node.IsSequence() && node.size() == offset + 4, "Invalid replay TLBR rectangle");
  hm::BBox box(
      node[offset].as<float>(),
      node[offset + 1].as<float>(),
      node[offset + 2].as<float>(),
      node[offset + 3].as<float>());
  validate_box(box);
  return box;
}
bool same_box(const hm::BBox& a, const hm::BBox& b, double tolerance = 0) {
  return std::abs(double(a.left) - b.left) <= tolerance && std::abs(double(a.top) - b.top) <= tolerance &&
      std::abs(double(a.right) - b.right) <= tolerance && std::abs(double(a.bottom) - b.bottom) <= tolerance;
}

class SparseCsv {
 public:
  SparseCsv(const std::string& path, const std::atomic<bool>* cancelled, size_t columns)
      : lines_(path, cancelled), columns_(columns) {
    advance();
  }
  std::vector<std::vector<std::string>> take(uint64_t sample) {
    require(next_.empty() || u64(next_[0]) >= sample, "CSV row has no matching frame-index sample");
    std::vector<std::vector<std::string>> result;
    while (!next_.empty() && u64(next_[0]) == sample) {
      require(result.size() < 4096, "Recording frame exceeds 4096 tracks");
      result.push_back(std::move(next_));
      advance();
    }
    return result;
  }

 private:
  void advance() {
    std::string line;
    if (!lines_.next(&line)) {
      next_.clear();
      return;
    }
    next_ = csv(line);
    require(next_.size() == columns_, "Incorrect recording CSV column count");
    const auto sample = u64(next_[0]);
    require(sample > 0 && sample >= previous_, "CSV sample IDs must be monotonically ordered");
    previous_ = sample;
  }
  Lines lines_;
  size_t columns_;
  uint64_t previous_{0};
  std::vector<std::string> next_;
};
struct Sample {
  Frame original;
  uint64_t source{0}, seek{0}, reset{0};
  uint32_t width{0}, height{0};
  hm::BBox arena;
  bool stepped{true};
  bool has_received_tracks{false};
  std::vector<size_t> ids;
  std::vector<hm::BBox> boxes;
  std::string checkpoint, base_checkpoint;
};
struct StartingState {
  Snapshot snapshot;
  hm::play_tracker::PlayTrackerConfig base;
  bool has_received_tracks{false};
  Frame previous;
};
Tracker restore(const StartingState& state) {
  Tracker tracker;
  tracker.play_tracker = hm::play_tracker::PlayTracker::from_snapshot(state.snapshot);
  tracker.base_play_tracker_config = state.base;
  tracker.play_tracker_config = state.snapshot.config;
  tracker.play_tracker_config.living_boxes.clear();
  for (const auto& box : state.snapshot.living_boxes)
    tracker.play_tracker_config.living_boxes.push_back(box.config);
  if (state.snapshot.detector.overshoot_stop_delay_override)
    tracker.play_tracker_config.play_detector.overshoot_stop_delay_count =
        *state.snapshot.detector.overshoot_stop_delay_override;
  if (state.snapshot.detector.overshoot_scale_override)
    tracker.play_tracker_config.play_detector.overshoot_scale_speed_ratio =
        *state.snapshot.detector.overshoot_scale_override;
  tracker.has_received_tracks = state.has_received_tracks;
  return tracker;
}
StartingState capture(const Tracker& tracker, const Frame& previous) {
  return {tracker.play_tracker->snapshot(), tracker.base_play_tracker_config, tracker.has_received_tracks, previous};
}
Frame step(Tracker* tracker, const Sample& sample, const Frame& previous) {
  Frame result = sample.original;
  result.fast = previous.fast;
  result.follower = previous.follower;
  if (sample.stepped) {
    const auto output = DsPlayTrackerStepCpu(tracker, sample.ids, sample.boxes);
    result.fast.reset();
    result.follower.reset();
    if (!output.tracking_boxes.empty()) {
      result.fast = output.tracking_boxes.front();
      result.follower = output.tracking_boxes.back();
    }
  }
  return result;
}
void parity(const Frame& expected, const Frame& actual, double tolerance) {
  const auto close = [tolerance](const auto& a, const auto& b) {
    return a.has_value() == b.has_value() && (!a || same_box(*a, *b, tolerance));
  };
  require(
      close(expected.fast, actual.fast) && close(expected.follower, actual.follower),
      "Historical state could not be verified at sample " + std::to_string(expected.sample_id) +
          ". Both camera trajectories must agree within " + std::to_string(tolerance) +
          " pixels. Use the original arena/configuration and a compatible native implementation, or a new replay capture.");
}

YAML::Node tuning_node(const DsPlayTrackerRuntimeTuning& tuning) {
  YAML::Node document;
  auto p = document["play-tracker"];
  p["hstream-apply-to-fast-box"] = tuning.apply_to_fast_box;
  p["hstream-apply-to-follower-box"] = tuning.apply_to_follower_box;
  p["live-boxes"].push_back(YAML::Node(YAML::NodeType::Map));
  p["live-boxes"].push_back(YAML::Node(YAML::NodeType::Map));
  auto t = p["hstream-runtime-tuning"];
#define TUNING_FIELD(field, key) \
  if (tuning.field)              \
  t[key] = *tuning.field
  TUNING_FIELD(stop_on_dir_change_delay, "stop-translation-on-dir-change-delay");
  TUNING_FIELD(cancel_on_opposite, "cancel-stop-on-opposite-dir");
  TUNING_FIELD(cancel_hysteresis_frames, "cancel-stop-hysteresis-frames");
  TUNING_FIELD(stop_delay_cooldown_frames, "stop-delay-cooldown-frames");
  TUNING_FIELD(post_nonstop_stop_delay_count, "post-nonstop-stop-delay-count");
  TUNING_FIELD(time_to_dest_speed_limit_frames, "time-to-dest-speed-limit-frames");
  TUNING_FIELD(overshoot_stop_delay_count, "overshoot-stop-delay-count");
  TUNING_FIELD(overshoot_scale_speed_ratio, "overshoot-scale-speed-ratio");
  TUNING_FIELD(max_speed_x, "max-speed-x");
  TUNING_FIELD(max_speed_y, "max-speed-y");
  TUNING_FIELD(max_accel_x, "max-accel-x");
  TUNING_FIELD(max_accel_y, "max-accel-y");
  TUNING_FIELD(zoom_in_aggressiveness, "zoom-in-aggressiveness");
  TUNING_FIELD(ignore_largest_bbox_count, "ignore-largest-bbox-count");
  TUNING_FIELD(ignore_oversized_bboxes, "ignore-oversized-bboxes");
  TUNING_FIELD(oversized_bbox_percent, "oversized-bbox-percent");
  TUNING_FIELD(arena_angle_from_vertical, "arena-angle-from-vertical");
  TUNING_FIELD(dynamic_acceleration_scaling, "dynamic-acceleration-scaling");
#undef TUNING_FIELD
  // Preserve an empty override map, which is a valid recomputed baseline.
  if (!p["hstream-runtime-tuning"])
    p["hstream-runtime-tuning"] = YAML::Node(YAML::NodeType::Map);
  return document;
}
void validate_tuning(const DsPlayTrackerRuntimeTuning& tuning) {
  const auto parsed = DsPlayTrackerLoadRuntimeTuningContents(YAML::Dump(tuning_node(tuning)));
  require(parsed.ok(), parsed.status().ToString());
  for (const auto value :
       {tuning.stop_on_dir_change_delay,
        tuning.cancel_hysteresis_frames,
        tuning.stop_delay_cooldown_frames,
        tuning.post_nonstop_stop_delay_count,
        tuning.time_to_dest_speed_limit_frames,
        tuning.overshoot_stop_delay_count})
    require(!value || *value >= 0, "Trial braking counts must be nonnegative");
  for (const auto value :
       {tuning.max_speed_x,
        tuning.max_speed_y,
        tuning.max_accel_x,
        tuning.max_accel_y,
        tuning.overshoot_scale_speed_ratio,
        tuning.dynamic_acceleration_scaling})
    require(!value || *value >= 0, "Trial speed, acceleration, and scaling must be nonnegative");
}
struct Event {
  uint64_t boundary;
  std::string kind, key, value, artifact;
};
std::vector<Event> events(const std::string& path, const std::atomic<bool>* cancelled) {
  Lines lines(path, cancelled);
  std::string line;
  require(lines.next(&line) && line == "Event,SampleBoundary,Kind,Key,Value,Artifact", "Invalid config-events header");
  std::vector<Event> result;
  uint64_t boundary = 0;
  size_t total_bytes = 0;
  while (lines.next(&line)) {
    total_bytes += line.size();
    require(total_bytes <= kMaxArtifact, "Configuration-event history exceeds 16 MiB");
    auto row = csv(line);
    require(row.size() == 6 && result.size() < 10000, "Invalid/excessive configuration events");
    const uint64_t current = u64(row[1]);
    require(current >= boundary, "Configuration events are out of order");
    boundary = current;
    result.push_back({current, row[2], row[3], row[4], row[5]});
  }
  return result;
}
} // namespace

struct ReplaySession::Impl {
  PrepareOptions options;
  std::string provenance;
  std::string manifest_contents;
  StartingState start;
  std::vector<Sample> samples;
  std::vector<Frame> original, baseline;
  uint32_t width{0}, height{0};
  uint64_t end_pts_ns{0};
};
ReplaySession::ReplaySession(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

absl::StatusOr<std::shared_ptr<ReplaySession>> ReplaySession::Prepare(
    const PrepareOptions& options,
    const std::atomic<bool>* cancelled) {
  try {
    check_cancel(cancelled);
    require(
        std::isfinite(options.start_seconds) && options.start_seconds >= 0 && std::isfinite(options.duration_seconds) &&
            options.duration_seconds > 0 && options.duration_seconds <= 120 && options.start_seconds < 1e8,
        "Select a finite range from 0 to 120 seconds with a nonnegative start");
    require(
        std::isfinite(options.legacy_tolerance_pixels) && options.legacy_tolerance_pixels >= 0 &&
            options.legacy_tolerance_pixels <= 1,
        "Legacy trajectory tolerance must be between 0 and 1 pixel");
    auto impl = std::make_shared<Impl>();
    impl->options = options;
    impl->options.manifest_path = fs::absolute(options.manifest_path).string();
    impl->manifest_contents = read_artifact(options.manifest_path);
    const auto manifest = YAML::Load(impl->manifest_contents);
    require(
        manifest["schema"].as<std::string>() == "hstream-playtracker-telemetry-v1",
        "Unsupported telemetry manifest schema");
    require(
        manifest["completed"].as<bool>() && manifest["writer_drained"].as<bool>() &&
            !manifest["writer_failed"].as<bool>() && manifest["publication_state"].as<std::string>() == "committed",
        "Experiments require a completed, durably committed telemetry recording");
    const fs::path directory = fs::absolute(options.manifest_path).parent_path();
    const auto sidecars = manifest["sidecars"];
    const bool exact = static_cast<bool>(sidecars["replay"]);
    Lines index(artifact_path(directory, sidecars["frame_index"]), cancelled);
    SparseCsv fast(artifact_path(directory, manifest["hm_compatibility"]["camera_fast_csv"]["file"]), cancelled, 5);
    SparseCsv follower(artifact_path(directory, manifest["hm_compatibility"]["camera_csv"]["file"]), cancelled, 5);
    std::unique_ptr<Lines> replay;
    std::unique_ptr<SparseCsv> tracks;
    std::vector<Event> history_events;
    size_t event_index = 0;
    YAML::Node legacy_yaml;
    hm::BBox legacy_arena;
    std::vector<DsPlayTrackerRuntimeTuning> accumulated;
    float left_angle = 0, right_angle = 0, acceleration_scaling = 0;
    if (exact) {
      replay = std::make_unique<Lines>(artifact_path(directory, sidecars["replay"]), cancelled);
      impl->provenance = "Restored native checkpoint and exact recorded policy inputs";
    } else {
      require(
          options.legacy_arena.has_value(),
          "Legacy CSVs do not contain the historical arena. Supply the original arena rectangle explicitly.");
      legacy_arena = *options.legacy_arena;
      validate_box(legacy_arena);
      const std::string config_path = options.legacy_config_path.empty()
          ? artifact_path(directory, manifest["config_provenance"]["effective_artifact"])
          : options.legacy_config_path;
      legacy_yaml = YAML::Load(read_artifact(config_path))["play-tracker"];
      require(legacy_yaml && legacy_yaml.IsMap(), "Original effective configuration is missing play-tracker");
      tracks = std::make_unique<SparseCsv>(
          artifact_path(directory, manifest["hm_compatibility"]["tracking_csv"]["file"]), cancelled, 13);
      history_events = events(artifact_path(directory, sidecars["config_events"]), cancelled);
      require(
          !history_events.empty() && history_events.front().boundary == 1 &&
              history_events.front().kind == "base-config",
          "Legacy history must contain the original initialization event at sample 1");
      impl->provenance = "Legacy CSV history reconstructed and verified against both recorded camera trajectories (" +
          std::to_string(options.legacy_tolerance_pixels) + " px tolerance); explicit historical arena";
      const auto boxes = legacy_yaml["live-boxes"];
      if (boxes && boxes.IsSequence() && boxes.size()) {
        const auto last = boxes[boxes.size() - 1];
        if (last["arena-angle-from-vertical"])
          left_angle = right_angle = last["arena-angle-from-vertical"].as<float>();
        if (last["dynamic-acceleration-scaling"])
          acceleration_scaling = last["dynamic-acceleration-scaling"].as<float>();
      }
    }
    std::string line;
    require(
        index.next(&line) &&
            line ==
                "Sample,SourceID,SourceFrame,DecodedSourceID,DecodedSequence,PTS_NS,NTP_NS,SeekEpoch,Width,Height,DetectionCount,TrackCount,HasCamera",
        "Invalid frame-index header");
    std::optional<Tracker> reference;
    std::optional<Tracker> baseline;
    Frame previous_reference, previous_baseline;
    std::optional<Sample> previous_sample;
    uint64_t origin = 0, in = 0, out = 0;
    bool started = false, reached_out = false;
    size_t retained_tracks = 0;
    while (index.next(&line)) {
      check_cancel(cancelled);
      const auto row = csv(line);
      require(row.size() == 13, "Invalid frame-index column count");
      Sample sample;
      sample.original.sample_id = u64(row[0]);
      sample.source = u64(row[1]);
      sample.original.pts_ns = u64(row[5]);
      sample.seek = u64(row[7]);
      const auto width = u64(row[8]), height = u64(row[9]), track_count = u64(row[11]);
      require(
          width > 0 && width <= 32768 && height > 0 && height <= 32768 && track_count <= 4096,
          "Invalid canvas size or excessive track count in recording");
      sample.width = width;
      sample.height = height;
      require(
          sample.original.sample_id > 0 &&
              (!previous_sample || sample.original.sample_id > previous_sample->original.sample_id),
          "Frame-index sample IDs must be strictly increasing");
      if (!previous_sample) {
        origin = sample.original.pts_ns;
        require(
            origin < std::numeric_limits<uint64_t>::max() - uint64_t((options.start_seconds + 121) * 1e9),
            "Recording timestamp overflows range");
        in = origin + uint64_t(options.start_seconds * 1e9);
        out = in + uint64_t(options.duration_seconds * 1e9);
      }
      const auto fast_rows = fast.take(sample.original.sample_id);
      const auto follower_rows = follower.take(sample.original.sample_id);
      require(
          fast_rows.size() <= 1 && follower_rows.size() <= 1 && fast_rows.size() == follower_rows.size(),
          "Camera recording requires matching, unique fast and follower rows");
      require((u64(row[12]) != 0) == !follower_rows.empty(), "Camera availability disagrees with frame index");
      if (!fast_rows.empty()) {
        sample.original.fast = tlwh(fast_rows.front(), 1);
        sample.original.follower = tlwh(follower_rows.front(), 1);
      }
      if (exact) {
        require(replay->next(&line), "Replay sidecar ends before frame index");
        const auto record = YAML::Load(line);
        require(
            record["schema"].as<int>() == 1 && record["sample_id"].as<uint64_t>() == sample.original.sample_id &&
                record["source_id"].as<uint64_t>() == sample.source &&
                record["pts_ns"].as<uint64_t>() == sample.original.pts_ns &&
                record["seek_epoch"].as<uint64_t>() == sample.seek && record["width"].as<uint32_t>() == sample.width &&
                record["height"].as<uint32_t>() == sample.height,
            "Replay sidecar identity does not match frame index");
        sample.reset = record["reset_epoch"].as<uint64_t>();
        sample.arena = box_node(record["arena"]);
        sample.stepped = record["stepped"].as<bool>();
        sample.has_received_tracks = record["has_received_tracks"].as<bool>();
        const auto inputs = record["tracks"];
        require(
            inputs.IsSequence() && inputs.size() <= 4096 && (!sample.stepped || inputs.size() == track_count),
            "Replay policy input count disagrees with frame index");
        for (const auto input : inputs) {
          require(input.IsSequence() && input.size() == 5, "Invalid replay policy input");
          sample.ids.push_back(input[0].as<uint64_t>());
          sample.boxes.push_back(box_node(input, 1));
        }
        if (record["checkpoint"]) {
          sample.checkpoint = record["checkpoint"].as<std::string>();
          sample.base_checkpoint = record["base_checkpoint"].as<std::string>();
          require(!sample.checkpoint.empty() && !sample.base_checkpoint.empty(), "Empty native replay checkpoint");
        }
        if (record["edge_rotation_left"])
          sample.original.edge_rotation_left = record["edge_rotation_left"].as<float>();
        if (record["edge_rotation_right"])
          sample.original.edge_rotation_right = record["edge_rotation_right"].as<float>();
        require(
            std::isfinite(sample.original.edge_rotation_left) && std::isfinite(sample.original.edge_rotation_right),
            "Non-finite replay edge rotation");
      } else {
        sample.arena = legacy_arena;
        const auto track_rows = tracks->take(sample.original.sample_id);
        require(track_rows.size() == track_count, "Tracking CSV count disagrees with frame index");
        for (const auto& tracked : track_rows) {
          sample.ids.push_back(u64(tracked[1]));
          sample.boxes.push_back(tlwh(tracked, 2));
        }
        sample.reset = previous_sample ? previous_sample->reset : 0;
      }
      require(
          sample.arena.left >= 0 && sample.arena.top >= 0 && sample.arena.right <= sample.width &&
              sample.arena.bottom <= sample.height,
          "Historical arena lies outside the recorded canvas");
      bool boundary = previous_sample &&
          (sample.source != previous_sample->source || sample.seek != previous_sample->seek ||
           sample.reset != previous_sample->reset || sample.width != previous_sample->width ||
           sample.height != previous_sample->height || !same_box(sample.arena, previous_sample->arena));
      if (!exact) {
        while (event_index < history_events.size() &&
               history_events[event_index].boundary <= sample.original.sample_id) {
          const auto& event = history_events[event_index++];
          if (event.kind == "seek") {
            boundary = previous_sample.has_value();
            reference.reset();
          } else if (event.kind == "base-config") {
            if (event.boundary != 1) {
              legacy_yaml =
                  YAML::Load(read_artifact(artifact_path(directory, YAML::Node(event.artifact))))["play-tracker"];
              for (auto box : legacy_yaml["live-boxes"])
                box["arena-angle-from-vertical"] = 0.5f * (left_angle + right_angle);
              const auto live_boxes = legacy_yaml["live-boxes"];
              require(live_boxes.IsSequence() && live_boxes.size(), "Reloaded config has no live boxes");
              legacy_yaml["live-boxes"][live_boxes.size() - 1]["dynamic-acceleration-scaling"] = acceleration_scaling;
              ++sample.reset;
              boundary = previous_sample.has_value();
              reference.reset();
            }
          } else {
            DsPlayTrackerRuntimeTuning tuning;
            if (event.kind == "runtime-tuning") {
              const auto parsed = DsPlayTrackerLoadRuntimeTuningContents(
                  read_artifact(artifact_path(directory, YAML::Node(event.artifact))));
              require(parsed.ok(), parsed.status().ToString());
              tuning = *parsed;
            } else if (event.kind == "property") {
              tuning.update_motion_tuning = false;
              if (event.key == "dynamic-acceleration-scaling") {
                acceleration_scaling = finite_float(event.value);
                tuning.dynamic_acceleration_scaling = acceleration_scaling;
              } else {
                const float angle = finite_float(event.value);
                require(
                    event.key == "fixed-edge-rotation-angle" || event.key == "fixed-edge-rotation-angle-left" ||
                        event.key == "fixed-edge-rotation-angle-right",
                    "Unsupported historical runtime property: " + event.key);
                if (event.key != "fixed-edge-rotation-angle-right")
                  left_angle = angle;
                if (event.key != "fixed-edge-rotation-angle-left")
                  right_angle = angle;
                tuning.apply_to_fast_box = true;
                tuning.arena_angle_from_vertical = 0.5f * (left_angle + right_angle);
              }
            } else {
              throw ReplayError("Unsupported historical configuration event: " + event.kind);
            }
            validate_tuning(tuning);
            accumulated.push_back(tuning);
            if (reference) {
              const auto status = DsPlayTrackerApplyCpuRuntimeTuning(&*reference, tuning);
              require(status.ok(), status.ToString());
            }
          }
        }
        sample.original.edge_rotation_left = left_angle;
        sample.original.edge_rotation_right = right_angle;
      }
      // Out is an actual sample boundary, and is excluded from every trajectory.
      if (started && sample.original.pts_ns >= out) {
        impl->end_pts_ns = sample.original.pts_ns;
        reached_out = true;
        break;
      }
      if (started && (boundary || sample.original.pts_ns <= previous_sample->original.pts_ns))
        throw ReplayError("Selected range crosses a source, seek, reset, geometry, or nonmonotonic timeline boundary");
      if (boundary) {
        reference.reset();
        previous_reference = {};
      }
      if (exact && !sample.checkpoint.empty()) {
        StartingState state;
        state.snapshot = hm::play_tracker::deserialize_snapshot(sample.checkpoint);
        const auto base_snapshot = hm::play_tracker::deserialize_snapshot(sample.base_checkpoint);
        state.base = base_snapshot.config;
        state.has_received_tracks = sample.has_received_tracks;
        require(
            state.base.living_boxes.size() == state.snapshot.living_boxes.size(),
            "Checkpoint base live-box topology differs");
        for (size_t i = 0; i < state.base.living_boxes.size(); ++i) {
          const auto& box = state.base.living_boxes[i];
          require(
              box.arena_box && same_box(*box.arena_box, sample.arena) &&
                  box.name == state.snapshot.living_boxes[i].config.name,
              "Checkpoint base arena or live-box order differs from effective state");
        }
        for (const auto& box : state.snapshot.living_boxes)
          require(
              box.config.arena_box && same_box(*box.config.arena_box, sample.arena),
              "Checkpoint arena differs from replay sample");
        reference = restore(state);
      }
      if (!reference && exact)
        throw ReplayError("A native checkpoint is required at the recording/segment initialization boundary");
      if (!reference) {
        auto created = DsPlayTrackerCreateCpuTracker(sample.arena, legacy_yaml);
        require(created.ok(), created.status().ToString());
        reference = std::move(*created);
        for (const auto& tuning : accumulated) {
          const auto status = DsPlayTrackerApplyCpuRuntimeTuning(&*reference, tuning);
          require(status.ok(), status.ToString());
        }
      }
      if (exact)
        require(
            sample.has_received_tracks == reference->has_received_tracks,
            "Replay wrapper state disagrees with the preceding native history");
      if (!started && sample.original.pts_ns >= in) {
        impl->start = capture(*reference, previous_reference);
        baseline = restore(impl->start);
        previous_baseline = previous_reference;
        impl->width = sample.width;
        impl->height = sample.height;
        started = true;
      }
      previous_reference = step(&*reference, sample, previous_reference);
      parity(sample.original, previous_reference, exact ? 0.02 : options.legacy_tolerance_pixels);
      if (started) {
        retained_tracks += sample.ids.size();
        require(
            impl->samples.size() < kMaxRangeSamples && retained_tracks <= kMaxRangeTracks,
            "Selected range exceeds bounded replay sample/track storage; select a shorter range");
        previous_baseline = step(&*baseline, sample, previous_baseline);
        // A trial freezes camera geometry at its fork, just like its policy.
        previous_baseline.edge_rotation_left =
            impl->original.empty() ? sample.original.edge_rotation_left : impl->original.front().edge_rotation_left;
        previous_baseline.edge_rotation_right =
            impl->original.empty() ? sample.original.edge_rotation_right : impl->original.front().edge_rotation_right;
        impl->original.push_back(sample.original);
        impl->baseline.push_back(previous_baseline);
        sample.checkpoint.clear();
        sample.base_checkpoint.clear();
        impl->samples.push_back(sample);
      }
      previous_sample = std::move(sample);
    }
    require(started && !impl->samples.empty(), "Selected in point is outside the recording");
    require(reached_out, "Selected out point extends beyond the recording; choose a shorter range");
    check_cancel(cancelled);
    return std::shared_ptr<ReplaySession>(new ReplaySession(std::move(impl)));
  } catch (const std::exception& error) {
    if (cancelled && cancelled->load(std::memory_order_relaxed))
      return absl::CancelledError("Replay cancelled");
    return absl::InvalidArgumentError(error.what());
  }
}

absl::StatusOr<TrialResult> ReplaySession::RunTrial(
    std::string name,
    const DsPlayTrackerRuntimeTuning& tuning,
    const std::atomic<bool>* cancelled) const {
  try {
    check_cancel(cancelled);
    validate_tuning(tuning);
    Tracker tracker = restore(impl_->start);
    const auto status = DsPlayTrackerApplyCpuRuntimeTuning(&tracker, tuning);
    require(status.ok(), status.ToString());
    hm::play_tracker::validate_snapshot(tracker.play_tracker->snapshot());
    TrialResult result{std::move(name), tuning, {}};
    Frame previous = impl_->start.previous;
    for (const auto& sample : impl_->samples) {
      check_cancel(cancelled);
      previous = step(&tracker, sample, previous);
      previous.edge_rotation_left = impl_->original.front().edge_rotation_left;
      previous.edge_rotation_right = impl_->original.front().edge_rotation_right;
      result.frames.push_back(previous);
    }
    return result;
  } catch (const std::exception& error) {
    if (cancelled && cancelled->load(std::memory_order_relaxed))
      return absl::CancelledError("Replay cancelled");
    return absl::InvalidArgumentError(error.what());
  }
}

const std::vector<Frame>& ReplaySession::original() const {
  return impl_->original;
}
const std::vector<Frame>& ReplaySession::baseline() const {
  return impl_->baseline;
}
uint32_t ReplaySession::width() const {
  return impl_->width;
}
uint32_t ReplaySession::height() const {
  return impl_->height;
}
uint64_t ReplaySession::end_pts_ns() const {
  return impl_->end_pts_ns;
}
const std::string& ReplaySession::provenance() const {
  return impl_->provenance;
}
const std::string& ReplaySession::manifest_path() const {
  return impl_->options.manifest_path;
}

absl::Status ReplaySession::SaveTrial(const std::string& path, const TrialResult& trial, const MediaBinding& media)
    const {
  try {
    require(
        trial.frames.size() == impl_->samples.size() && !trial.frames.empty(), "Trial does not belong to this range");
    require(
        media.width == impl_->width && media.height == impl_->height,
        "Bind an uncropped panorama with the recording's canvas dimensions");
    require(fs::is_regular_file(media.path), "Bound panorama file is missing");
    const auto recording = YAML::Load(impl_->manifest_contents);
    const fs::path directory = fs::path(impl_->options.manifest_path).parent_path();
    const auto protects = [&path](const std::string& artifact) {
      return fs::exists(path) && fs::exists(artifact) && fs::equivalent(path, artifact);
    };
    require(
        !protects(media.path) && !protects(impl_->options.manifest_path),
        "A trial descriptor cannot replace its source media or recording manifest");
    for (const auto& artifact : recording["sidecars"])
      require(
          !protects(artifact_path(directory, artifact.second)), "A trial descriptor cannot replace recording sidecars");
    for (const auto& artifact : recording["hm_compatibility"]) {
      if (artifact.second.IsMap() && artifact.second["file"])
        require(
            !protects(artifact_path(directory, artifact.second["file"])),
            "A trial descriptor cannot replace training CSVs");
    }
    for (const char* key : {"source_artifact", "effective_artifact"}) {
      if (recording["config_provenance"][key])
        require(
            !protects(artifact_path(directory, recording["config_provenance"][key])),
            "A trial descriptor cannot replace original configuration artifacts");
    }
    YAML::Node document;
    document["schema"] = "hstream-camera-experiment-v1";
    document["name"] = trial.name;
    document["manifest"] = impl_->options.manifest_path;
    document["manifest_contents"] = impl_->manifest_contents;
    document["provenance"] = impl_->provenance;
    document["first_sample"] = impl_->original.front().sample_id;
    document["in_pts_ns"] = impl_->original.front().pts_ns;
    document["out_pts_ns"] = impl_->end_pts_ns;
    document["source_id"] = impl_->samples.front().source;
    document["seek_epoch"] = impl_->samples.front().seek;
    document["reset_epoch"] = impl_->samples.front().reset;
    for (const float coordinate :
         {impl_->samples.front().arena.left,
          impl_->samples.front().arena.top,
          impl_->samples.front().arena.right,
          impl_->samples.front().arena.bottom})
      document["arena"].push_back(coordinate);
    document["start_checkpoint"] = hm::play_tracker::serialize_snapshot(impl_->start.snapshot);
    document["start_has_received_tracks"] = impl_->start.has_received_tracks;
    hm::play_tracker::PlayTracker base(impl_->samples.front().arena, impl_->start.base);
    document["base_checkpoint"] = hm::play_tracker::serialize_snapshot(base.snapshot());
    document["tuning"] = tuning_node(trial.tuning);
    document["update_motion_tuning"] = trial.tuning.update_motion_tuning;
    auto binding = document["media"];
    binding["path"] = fs::absolute(media.path).string();
    binding["size_bytes"] = fs::file_size(media.path);
    binding["modified_ticks"] = fs::last_write_time(media.path).time_since_epoch().count();
    binding["width"] = media.width;
    binding["height"] = media.height;
    binding["telemetry_origin_pts_ns"] = media.telemetry_origin_pts_ns;
    binding["video_origin_pts_ns"] = media.video_origin_pts_ns;
    binding["identity"] = "Manually bound uncropped panorama; content identity requires operator verification";
    for (size_t i = 0; i < trial.frames.size(); ++i) {
      const auto& frame = trial.frames[i];
      const auto& sample = impl_->samples[i];
      require(
          frame.sample_id == sample.original.sample_id && frame.pts_ns == sample.original.pts_ns,
          "Trial frame identity differs from prepared session");
      YAML::Node row;
      row["sample_id"] = frame.sample_id;
      row["pts_ns"] = frame.pts_ns;
      row["stepped"] = sample.stepped;
      for (size_t j = 0; j < sample.ids.size(); ++j) {
        YAML::Node track;
        track.push_back(uint64_t(sample.ids[j]));
        for (const float coordinate :
             {sample.boxes[j].left, sample.boxes[j].top, sample.boxes[j].right, sample.boxes[j].bottom})
          track.push_back(coordinate);
        row["tracks"].push_back(track);
      }
      for (const auto& pair : {std::make_pair("fast", frame.fast), std::make_pair("follower", frame.follower)}) {
        if (pair.second) {
          for (const float coordinate : {pair.second->left, pair.second->top, pair.second->right, pair.second->bottom})
            row[pair.first].push_back(coordinate);
        }
      }
      row["edge_rotation_left"] = frame.edge_rotation_left;
      row["edge_rotation_right"] = frame.edge_rotation_right;
      document["frames"].push_back(row);
    }
    YAML::Emitter emitter;
    emitter.SetFloatPrecision(std::numeric_limits<float>::max_digits10);
    emitter.SetDoublePrecision(std::numeric_limits<double>::max_digits10);
    emitter << document;
    require(emitter.good(), "Could not serialize trial descriptor");
    struct Temporary {
      std::string path;
      int fd{-1};
      ~Temporary() {
        if (fd >= 0)
          ::close(fd);
        if (!path.empty())
          ::unlink(path.c_str());
      }
    } temporary{path + ".tmp-XXXXXX"};
    temporary.fd = ::mkstemp(temporary.path.data());
    require(temporary.fd >= 0, "Could not create trial descriptor: " + path);
    const std::string contents = std::string(emitter.c_str()) + '\n';
    size_t written = 0;
    while (written < contents.size()) {
      const ssize_t count = ::write(temporary.fd, contents.data() + written, contents.size() - written);
      if (count < 0 && errno == EINTR)
        continue;
      require(count > 0, "Could not write trial descriptor: " + path);
      written += count;
    }
    require(::fsync(temporary.fd) == 0, "Could not synchronize trial descriptor: " + path);
    require(::rename(temporary.path.c_str(), path.c_str()) == 0, "Could not commit trial descriptor: " + path);
    temporary.path.clear();
    return absl::OkStatus();
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  }
}
} // namespace hm::playtracker_replay
