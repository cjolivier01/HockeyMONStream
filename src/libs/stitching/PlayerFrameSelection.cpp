#include "hstream/src/libs/stitching/PlayerFrameSelection.h"

#include <openssl/evp.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace hm::stitching {
namespace {
constexpr size_t kMaximumDocumentBytes = 1024 * 1024;
constexpr size_t kMaximumSources = 256;
constexpr size_t kMaximumContextBytes = 128 * 1024;
constexpr const char* kAlgorithm = "ice-mask-overlap-coverage-v1";

absl::Status invalid(const std::string& message) {
  return absl::InvalidArgumentError("Player frame selection: " + message);
}

bool local_path(const std::string& path) {
  return !path.empty() && path.size() <= 4096 && path.find('\0') == std::string::npos &&
      std::filesystem::path(path).is_absolute() && std::filesystem::path(path).lexically_normal().string() == path;
}

absl::Status validate_pair(const PlayerFramePairIdentity& pair) {
  if (pair.timeline_pts_ns == std::numeric_limits<uint64_t>::max())
    return invalid("invalid pair timeline PTS");
  for (const auto& frame : pair.cameras) {
    if (!local_path(frame.path) || frame.source_pts_ns == std::numeric_limits<uint64_t>::max())
      return invalid("frames require normalized absolute local paths and valid integer source PTS");
  }
  if (pair.cameras[0].path == pair.cameras[1].path)
    return invalid("both cameras cannot reference the same physical source");
  return absl::OkStatus();
}

bool same_pair(const PlayerFramePairIdentity& a, const PlayerFramePairIdentity& b) {
  for (size_t i = 0; i < 2; ++i) {
    if (a.cameras[i].path != b.cameras[i].path || a.cameras[i].source_pts_ns != b.cameras[i].source_pts_ns)
      return false;
  }
  return true;
}

absl::Status validate_observation(const PlayerFrameObservation& observation) {
  auto status = validate_pair(observation.pair);
  if (!status.ok())
    return status;
  if (observation.eligible_people > 256 || !std::isfinite(observation.quality) || observation.quality < 0 ||
      observation.quality > 4 || observation.coverage.size() > kPlayerFrameCoverageCells * 3)
    return invalid("observation exceeds scoring bounds");
  size_t count = 0;
  for (size_t band : observation.size_band_counts) {
    if (band > 256)
      return invalid("invalid size-band count");
    count += band;
  }
  if (count != observation.eligible_people ||
      !std::is_sorted(observation.coverage.begin(), observation.coverage.end()) ||
      std::adjacent_find(observation.coverage.begin(), observation.coverage.end()) != observation.coverage.end())
    return invalid("invalid observation counts or coverage ordering");
  for (uint16_t cell : observation.coverage) {
    if (cell >= 3 * kPlayerFrameCoverageCells || observation.size_band_counts[cell / kPlayerFrameCoverageCells] == 0)
      return invalid("coverage refers to an unobserved size band");
  }
  if ((observation.eligible_people == 0) != observation.coverage.empty())
    return invalid("person counts and coverage disagree");
  return absl::OkStatus();
}

absl::Status validate_context(const PlayerFrameSelectionContext& context) {
  if (context.size() > 64)
    return invalid("too many context entries");
  size_t bytes = 0;
  for (const auto& [key, value] : context) {
    if (key.empty() || key.size() > 128 || value.empty() || key.find('\0') != std::string::npos ||
        value.find('\0') != std::string::npos)
      return invalid("invalid context key/value");
    bytes += key.size() + value.size();
  }
  if (bytes > kMaximumContextBytes)
    return invalid("context is too large");
  for (const char* key :
       {"source_context",
        "baseline_generation",
        "output_generation",
        "detector_identity",
        "rink_mask_sha256",
        "rink_mask_revision",
        "fieldmask_settings",
        "output_rotation_degrees"}) {
    if (context.find(key) == context.end())
      return invalid(std::string("missing context: ") + key);
  }
  return absl::OkStatus();
}

absl::Status validate_plan_fields(const PlayerFrameSelectionPlan& plan) {
  auto status = ValidatePlayerFrameSelectionSettings(plan.settings);
  if (!status.ok())
    return status;
  status = validate_context(plan.context);
  if (!status.ok())
    return status;
  if (plan.sources.empty() || plan.sources.size() > kMaximumSources ||
      plan.selected.size() != plan.settings.frame_count)
    return invalid("source binding or selected pair count is invalid");
  std::set<std::string> sources;
  for (const auto& source : plan.sources) {
    if (!local_path(source.path) || source.size == 0 || !sources.insert(source.path).second)
      return invalid("invalid/duplicate source binding");
  }
  std::set<std::tuple<std::string, uint64_t, std::string, uint64_t>> unique;
  for (size_t i = 0; i < plan.selected.size(); ++i) {
    const auto& observation = plan.selected[i];
    status = validate_observation(observation);
    if (!status.ok())
      return status;
    const auto& pair = observation.pair;
    for (const auto& camera : pair.cameras) {
      if (!sources.count(camera.path))
        return invalid("selected frame has no source binding");
    }
    if (!unique
             .emplace(
                 pair.cameras[0].path,
                 pair.cameras[0].source_pts_ns,
                 pair.cameras[1].path,
                 pair.cameras[1].source_pts_ns)
             .second)
      return invalid("duplicate selected pair");
    if (i > 0) {
      const uint64_t previous = plan.selected[i - 1].pair.timeline_pts_ns;
      if (pair.timeline_pts_ns <= previous || pair.timeline_pts_ns - previous < plan.settings.minimum_separation_ns ||
          pair.timeline_pts_ns - plan.selected.front().pair.timeline_pts_ns > plan.settings.duration_ns)
        return invalid("selected pairs violate ordering, separation or search duration");
      if (observation.eligible_people == 0)
        return invalid("selected non-anchor frame has no eligible people");
    }
  }
  return absl::OkStatus();
}

YAML::Node settings_yaml(const PlayerFrameSelectionSettings& settings) {
  YAML::Node node;
  node["frame_count"] = settings.frame_count;
  node["duration_ns"] = settings.duration_ns;
  node["interval_ns"] = settings.interval_ns;
  node["minimum_separation_ns"] = settings.minimum_separation_ns;
  node["maximum_observations"] = settings.maximum_observations;
  return node;
}

YAML::Node observation_yaml(const PlayerFrameObservation& observation) {
  YAML::Node node;
  node["timeline_pts_ns"] = observation.pair.timeline_pts_ns;
  for (const auto& frame : observation.pair.cameras) {
    YAML::Node camera;
    camera["path"] = frame.path;
    camera["source_pts_ns"] = frame.source_pts_ns;
    camera["source_id"] = frame.source_id;
    camera["sequence"] = frame.sequence;
    node["cameras"].push_back(camera);
  }
  node["coverage"] = YAML::Node(YAML::NodeType::Sequence);
  for (auto cell : observation.coverage)
    node["coverage"].push_back(cell);
  for (auto count : observation.size_band_counts)
    node["size_band_counts"].push_back(count);
  node["eligible_people"] = observation.eligible_people;
  node["quality"] = observation.quality;
  return node;
}

YAML::Node plan_yaml(const PlayerFrameSelectionPlan& plan, bool fingerprint) {
  YAML::Node node;
  node["schema"] = 1;
  node["algorithm"] = kAlgorithm;
  node["settings"] = settings_yaml(plan.settings);
  // Source ordering is irrelevant; canonicalize for stable fingerprints.
  auto sources = plan.sources;
  std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
  for (const auto& source : sources) {
    YAML::Node binding;
    binding["path"] = source.path;
    binding["size"] = source.size;
    binding["modification_time_ns"] = source.modification_time_ns;
    node["sources"].push_back(binding);
  }
  for (const auto& [key, value] : plan.context)
    node["context"][key] = value;
  for (const auto& observation : plan.selected)
    node["selected"].push_back(observation_yaml(observation));
  if (fingerprint)
    node["fingerprint"] = plan.fingerprint;
  return node;
}

void keys(const YAML::Node& node, std::initializer_list<const char*> allowed) {
  if (!node.IsMap())
    throw std::invalid_argument("expected a map");
  std::set<std::string> permitted;
  for (const char* key : allowed)
    permitted.insert(key);
  std::set<std::string> seen;
  for (const auto& entry : node) {
    if (!entry.first.IsScalar())
      throw std::invalid_argument("non-scalar map key");
    const std::string key = entry.first.Scalar();
    if (!permitted.count(key) || !seen.insert(key).second)
      throw std::invalid_argument("unknown or duplicate key: " + key);
  }
  if (seen.size() != permitted.size())
    throw std::invalid_argument("missing required map field");
}

std::string string_value(const YAML::Node& node, size_t maximum = kMaximumContextBytes) {
  if (!node.IsScalar() || node.Scalar().size() > maximum || node.Scalar().find('\0') != std::string::npos)
    throw std::invalid_argument("invalid string value");
  return node.Scalar();
}

uint64_t uint_value(const YAML::Node& node) {
  const std::string value = string_value(node, 20);
  if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
    throw std::invalid_argument("expected a decimal unsigned integer");
  size_t used = 0;
  const uint64_t parsed = std::stoull(value, &used);
  if (used != value.size())
    throw std::invalid_argument("invalid integer");
  return parsed;
}

size_t bounded_size(const YAML::Node& node, size_t maximum) {
  const uint64_t value = uint_value(node);
  if (value > maximum)
    throw std::invalid_argument("integer exceeds bounds");
  return static_cast<size_t>(value);
}

PlayerFrameSelectionSettings parse_settings(const YAML::Node& node) {
  keys(node, {"frame_count", "duration_ns", "interval_ns", "minimum_separation_ns", "maximum_observations"});
  PlayerFrameSelectionSettings settings;
  settings.frame_count = bounded_size(node["frame_count"], kPlayerFrameMaximumPairs);
  settings.duration_ns = uint_value(node["duration_ns"]);
  settings.interval_ns = uint_value(node["interval_ns"]);
  settings.minimum_separation_ns = uint_value(node["minimum_separation_ns"]);
  settings.maximum_observations = bounded_size(node["maximum_observations"], 1200);
  return settings;
}

void sequence_size(const YAML::Node& node, size_t maximum) {
  if (!node.IsSequence() || node.size() > maximum)
    throw std::invalid_argument("sequence is missing or exceeds bounds");
}

PlayerFrameObservation parse_observation(const YAML::Node& node) {
  keys(node, {"timeline_pts_ns", "cameras", "coverage", "size_band_counts", "eligible_people", "quality"});
  PlayerFrameObservation result;
  result.pair.timeline_pts_ns = uint_value(node["timeline_pts_ns"]);
  sequence_size(node["cameras"], 2);
  if (node["cameras"].size() != 2)
    throw std::invalid_argument("exactly two cameras required");
  for (size_t i = 0; i < 2; ++i) {
    const YAML::Node camera = node["cameras"][i];
    keys(camera, {"path", "source_pts_ns", "source_id", "sequence"});
    result.pair.cameras[i] = {
        string_value(camera["path"], 4096),
        uint_value(camera["source_pts_ns"]),
        static_cast<uint32_t>(bounded_size(camera["source_id"], UINT32_MAX)),
        uint_value(camera["sequence"])};
  }
  sequence_size(node["coverage"], 3 * kPlayerFrameCoverageCells);
  for (const auto& cell : node["coverage"])
    result.coverage.push_back(static_cast<uint16_t>(bounded_size(cell, 3 * kPlayerFrameCoverageCells - 1)));
  sequence_size(node["size_band_counts"], 3);
  if (node["size_band_counts"].size() != 3)
    throw std::invalid_argument("exactly three size bands required");
  for (size_t i = 0; i < 3; ++i)
    result.size_band_counts[i] = bounded_size(node["size_band_counts"][i], 256);
  result.eligible_people = bounded_size(node["eligible_people"], 256);
  result.quality = node["quality"].as<double>();
  return result;
}
} // namespace

absl::Status ValidatePlayerFrameSelectionSettings(const PlayerFrameSelectionSettings& settings) {
  if (settings.frame_count < 1 || settings.frame_count > kPlayerFrameMaximumPairs || settings.duration_ns == 0 ||
      settings.duration_ns > 300 * kPlayerFrameSecond || settings.interval_ns == 0 ||
      settings.interval_ns > settings.duration_ns || settings.minimum_separation_ns == 0 ||
      settings.minimum_separation_ns > settings.duration_ns || settings.maximum_observations < 1 ||
      settings.maximum_observations > 1200 ||
      settings.duration_ns / settings.interval_ns + 1 > settings.maximum_observations)
    return invalid("settings exceed frame, duration, sampling or observation bounds");
  return absl::OkStatus();
}

absl::StatusOr<PlayerFrameSourceBinding> BindPlayerFrameSource(const std::filesystem::path& path) {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  if (error || !local_path(canonical.string()))
    return absl::FailedPreconditionError("Cannot resolve player-selection source: " + path.string());
  struct stat metadata{};
  if (::stat(canonical.c_str(), &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size <= 0)
    return absl::FailedPreconditionError(
        "Player-selection source is not a nonempty regular recording: " + path.string());
  const long double timestamp =
      static_cast<long double>(metadata.st_mtim.tv_sec) * kPlayerFrameSecond + metadata.st_mtim.tv_nsec;
  if (timestamp < std::numeric_limits<int64_t>::min() || timestamp > std::numeric_limits<int64_t>::max())
    return invalid("source modification timestamp overflows nanoseconds");
  return PlayerFrameSourceBinding{
      canonical.string(), static_cast<uint64_t>(metadata.st_size), static_cast<int64_t>(timestamp)};
}

absl::Status CanonicalizePlayerFramePair(PlayerFramePairIdentity* pair) {
  if (!pair)
    return invalid("null pair");
  for (auto& frame : pair->cameras) {
    auto source = BindPlayerFrameSource(frame.path);
    if (!source.ok())
      return source.status();
    frame.path = source->path;
  }
  return validate_pair(*pair);
}

absl::Status ValidatePlayerFrameSources(const PlayerFrameSelectionPlan& plan) {
  auto status = ValidatePlayerFrameSelectionPlan(plan);
  if (!status.ok())
    return status;
  for (const auto& expected : plan.sources) {
    auto actual = BindPlayerFrameSource(expected.path);
    if (!actual.ok())
      return actual.status();
    if (actual->path != expected.path || actual->size != expected.size ||
        actual->modification_time_ns != expected.modification_time_ns)
      return absl::FailedPreconditionError("Player-selection source changed: " + expected.path);
  }
  return absl::OkStatus();
}

absl::Status ValidatePlayerFrameSourceContext(const PlayerFrameSelectionPlan& plan, const std::string& expected) {
  auto status = ValidatePlayerFrameSelectionPlan(plan);
  if (!status.ok())
    return status;
  if (expected.empty() || plan.context.at("source_context") != expected)
    return absl::FailedPreconditionError("Player-selection camera/synchronization/anchor context changed");
  return absl::OkStatus();
}

absl::StatusOr<PlayerFrameObservation> ScorePlayerFrame(
    const PlayerFramePairIdentity& pair,
    const std::vector<PlayerFrameBox>& boxes,
    const cv::Mat& overlap_mask,
    cv::Size canvas_size) {
  auto status = validate_pair(pair);
  if (!status.ok())
    return status;
  if (canvas_size.width <= 0 || canvas_size.height <= 0 || overlap_mask.empty() || overlap_mask.type() != CV_8UC1 ||
      overlap_mask.total() > 2048 * 2048 || boxes.size() > 256)
    return invalid("invalid canvas, overlap mask or box count");
  PlayerFrameObservation result;
  result.pair = pair;
  std::set<uint16_t> coverage;
  double confidence_sum = 0;
  for (const auto& box : boxes) {
    if (box.class_id != 0)
      continue;
    if (!std::isfinite(box.left) || !std::isfinite(box.top) || !std::isfinite(box.width) ||
        !std::isfinite(box.height) || !std::isfinite(box.confidence) || box.width <= 0 || box.height <= 0 ||
        box.confidence < 0 || box.confidence > 1)
      return invalid("person box contains invalid coordinates/confidence");
    const double right = std::clamp(box.left + box.width, 0.0, static_cast<double>(canvas_size.width));
    const double bottom = std::clamp(box.top + box.height, 0.0, static_cast<double>(canvas_size.height));
    const double left = std::clamp(box.left, 0.0, static_cast<double>(canvas_size.width));
    const double top = std::clamp(box.top, 0.0, static_cast<double>(canvas_size.height));
    if (right <= left || bottom <= top)
      continue;
    const int x0 = std::clamp(
        static_cast<int>(std::floor(left * overlap_mask.cols / canvas_size.width)), 0, overlap_mask.cols - 1);
    const int x1 = std::clamp(
        static_cast<int>(std::ceil(right * overlap_mask.cols / canvas_size.width)), x0 + 1, overlap_mask.cols);
    const int y0 = std::clamp(
        static_cast<int>(std::floor(top * overlap_mask.rows / canvas_size.height)), 0, overlap_mask.rows - 1);
    const int y1 = std::clamp(
        static_cast<int>(std::ceil(bottom * overlap_mask.rows / canvas_size.height)), y0 + 1, overlap_mask.rows);
    std::array<size_t, kPlayerFrameCoverageCells> cells{};
    size_t visible = 0;
    for (int y = y0; y < y1; ++y) {
      const auto* row = overlap_mask.ptr<uint8_t>(y);
      for (int x = x0; x < x1; ++x) {
        if (row[x]) {
          ++visible;
          const size_t column = static_cast<size_t>(x) * 16 / overlap_mask.cols;
          const size_t cell_row = static_cast<size_t>(y) * 9 / overlap_mask.rows;
          ++cells[cell_row * 16 + column];
        }
      }
    }
    if (visible == 0 || static_cast<double>(visible) / ((x1 - x0) * (y1 - y0)) < 0.2)
      continue;
    const double relative_height = (bottom - top) / canvas_size.height;
    const size_t band = relative_height < 0.04 ? 0 : relative_height < 0.10 ? 1 : 2;
    ++result.eligible_people;
    ++result.size_band_counts[band];
    confidence_sum += box.confidence;
    std::vector<size_t> occupied;
    for (size_t i = 0; i < cells.size(); ++i) {
      if (cells[i])
        occupied.push_back(i);
    }
    std::sort(occupied.begin(), occupied.end(), [&](size_t a, size_t b) {
      return cells[a] != cells[b] ? cells[a] > cells[b] : a < b;
    });
    // At most four cells per person; selection rewards union coverage, not summed area/count.
    for (size_t i = 0; i < std::min<size_t>(4, occupied.size()); ++i)
      coverage.insert(static_cast<uint16_t>(band * kPlayerFrameCoverageCells + occupied[i]));
  }
  result.coverage.assign(coverage.begin(), coverage.end());
  if (result.eligible_people)
    result.quality = confidence_sum / result.eligible_people * std::min<size_t>(4, result.eligible_people);
  return result;
}

absl::StatusOr<PlayerFrameSelectionReport> SelectPlayerFrames(
    const PlayerFrameSelectionSettings& settings,
    const std::vector<PlayerFrameObservation>& observations,
    const std::vector<PlayerFrameSourceBinding>& sources,
    const PlayerFrameSelectionContext& context) {
  auto status = ValidatePlayerFrameSelectionSettings(settings);
  if (!status.ok())
    return status;
  status = validate_context(context);
  if (!status.ok())
    return status;
  if (observations.size() > settings.maximum_observations)
    return invalid("too many scan observations");
  if (sources.empty() || sources.size() > kMaximumSources)
    return invalid("invalid scan source bindings");
  std::set<std::string> source_paths;
  for (const auto& source : sources) {
    if (!local_path(source.path) || !source.size || !source_paths.insert(source.path).second)
      return invalid("invalid or duplicate scan source binding");
  }
  std::set<std::tuple<std::string, uint64_t, std::string, uint64_t>> unique_pairs;
  PlayerFrameSelectionReport report;
  report.observation_count = observations.size();
  for (size_t i = 0; i < observations.size(); ++i) {
    status = validate_observation(observations[i]);
    if (!status.ok())
      return status;
    const auto& pair = observations[i].pair;
    if (!source_paths.count(pair.cameras[0].path) || !source_paths.count(pair.cameras[1].path) ||
        !unique_pairs
             .emplace(
                 pair.cameras[0].path,
                 pair.cameras[0].source_pts_ns,
                 pair.cameras[1].path,
                 pair.cameras[1].source_pts_ns)
             .second)
      return invalid("observation has missing source bindings or repeats a physical pair");
    if (i &&
        (observations[i].pair.timeline_pts_ns <= observations[i - 1].pair.timeline_pts_ns ||
         observations[i].pair.timeline_pts_ns - observations.front().pair.timeline_pts_ns > settings.duration_ns))
      return invalid("scan observations are unordered or outside search duration");
    for (size_t band = 0; band < 3; ++band)
      report.observed_size_band_counts[band] += observations[i].size_band_counts[band];
  }
  if (observations.empty()) {
    report.unavailable_reason = "No synchronized inferred frames were observed before EOS";
    return report;
  }
  std::vector<size_t> selected{0};
  std::set<uint16_t> covered(observations.front().coverage.begin(), observations.front().coverage.end());
  std::array<bool, 3> bands{};
  for (size_t band = 0; band < 3; ++band)
    bands[band] = observations.front().size_band_counts[band] != 0;
  while (selected.size() < settings.frame_count) {
    std::optional<size_t> best;
    size_t best_gain = 0;
    for (size_t i = 1; i < observations.size(); ++i) {
      const auto& candidate = observations[i];
      if (!candidate.eligible_people)
        continue;
      const auto time = candidate.pair.timeline_pts_ns;
      bool separated = true;
      for (auto index : selected) {
        const auto other = observations[index].pair.timeline_pts_ns;
        if ((time > other ? time - other : other - time) < settings.minimum_separation_ns)
          separated = false;
      }
      if (!separated)
        continue;
      // Preserve enough remaining time-separated slots. Pure highest-score
      // greedy selection can otherwise strand a feasible requested frame count.
      std::vector<size_t> fixed = selected;
      fixed.push_back(i);
      std::sort(fixed.begin(), fixed.end());
      size_t attainable = fixed.size();
      for (size_t gap = 0; gap < fixed.size() && attainable < settings.frame_count; ++gap) {
        uint64_t previous = observations[fixed[gap]].pair.timeline_pts_ns;
        const size_t end = gap + 1 < fixed.size() ? fixed[gap + 1] : observations.size();
        for (size_t j = fixed[gap] + 1; j < end && attainable < settings.frame_count; ++j) {
          const auto& other = observations[j];
          if (!other.eligible_people || other.pair.timeline_pts_ns - previous < settings.minimum_separation_ns)
            continue;
          if (end < observations.size() &&
              observations[end].pair.timeline_pts_ns - other.pair.timeline_pts_ns < settings.minimum_separation_ns)
            break;
          ++attainable;
          previous = other.pair.timeline_pts_ns;
        }
      }
      if (attainable < settings.frame_count)
        continue;
      size_t gain = 0;
      for (uint16_t cell : candidate.coverage)
        gain += !covered.count(cell);
      for (size_t band = 0; band < 3; ++band)
        gain += (!bands[band] && candidate.size_band_counts[band] != 0) ? 4 : 0;
      if (!best || gain > best_gain || (gain == best_gain && candidate.quality > observations[*best].quality)) {
        best = i;
        best_gain = gain;
      }
    }
    if (!best) {
      report.unavailable_reason = "Insufficient separated ice-mask-filtered player frames for the requested count";
      return report;
    }
    selected.push_back(*best);
    covered.insert(observations[*best].coverage.begin(), observations[*best].coverage.end());
    for (size_t band = 0; band < 3; ++band)
      bands[band] = bands[band] || observations[*best].size_band_counts[band] != 0;
  }
  std::sort(selected.begin(), selected.end());
  PlayerFrameSelectionPlan plan;
  plan.settings = settings;
  plan.sources = sources;
  plan.context = context;
  for (size_t index : selected)
    plan.selected.push_back(observations[index]);
  auto fingerprint = PlayerFrameSelectionFingerprint(plan);
  if (!fingerprint.ok())
    return fingerprint.status();
  plan.fingerprint = *fingerprint;
  report.plan = std::move(plan);
  return report;
}

absl::StatusOr<std::string> PlayerFrameSelectionFingerprint(const PlayerFrameSelectionPlan& plan) {
  auto status = validate_plan_fields(plan);
  if (!status.ok())
    return status;
  YAML::Emitter emitter;
  emitter.SetDoublePrecision(std::numeric_limits<double>::max_digits10);
  emitter << plan_yaml(plan, false);
  if (!emitter.good())
    return absl::InternalError("Could not serialize player frame selection");
  const std::string bytes = emitter.c_str();
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 || size != 32)
    return absl::InternalError("Could not hash player frame selection");
  std::ostringstream output;
  output.imbue(std::locale::classic());
  for (unsigned int i = 0; i < size; ++i)
    output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(digest[i]);
  return output.str();
}

absl::Status ValidatePlayerFrameSelectionPlan(const PlayerFrameSelectionPlan& plan) {
  auto expected = PlayerFrameSelectionFingerprint(plan);
  if (!expected.ok())
    return expected.status();
  return plan.fingerprint == *expected ? absl::OkStatus() : invalid("plan fingerprint does not match its contents");
}

absl::StatusOr<std::string> PlayerFrameMaskFingerprint(const cv::Mat& mask) {
  if (mask.empty() || mask.type() != CV_8UC1 || mask.cols > 65536 || mask.rows > 65536 ||
      mask.total() > 256ULL * 1024 * 1024)
    return invalid("invalid rink mask for fingerprint");
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
    return absl::InternalError("Cannot initialize rink-mask hash");
  const std::string header =
      "hstream-player-rink-mask-v1:" + std::to_string(mask.cols) + ":" + std::to_string(mask.rows) + ":";
  if (EVP_DigestUpdate(digest.get(), header.data(), header.size()) != 1)
    return absl::InternalError("Cannot hash rink-mask dimensions");
  for (int y = 0; y < mask.rows; ++y) {
    if (EVP_DigestUpdate(digest.get(), mask.ptr<uint8_t>(y), mask.cols) != 1)
      return absl::InternalError("Cannot hash rink-mask pixels");
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
  unsigned int size = 0;
  if (EVP_DigestFinal_ex(digest.get(), bytes.data(), &size) != 1 || size != 32)
    return absl::InternalError("Cannot finalize rink-mask hash");
  std::ostringstream output;
  output.imbue(std::locale::classic());
  for (unsigned int i = 0; i < size; ++i)
    output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(bytes[i]);
  return output.str();
}

YAML::Node PlayerFrameSelectionPlanYaml(const PlayerFrameSelectionPlan& plan) {
  return plan_yaml(plan, true);
}

absl::StatusOr<PlayerFrameSelectionPlan> ParsePlayerFrameSelectionPlan(const YAML::Node& node) {
  try {
    keys(node, {"schema", "algorithm", "settings", "sources", "context", "selected", "fingerprint"});
    if (uint_value(node["schema"]) != 1 || string_value(node["algorithm"]) != kAlgorithm)
      return invalid("unsupported plan schema/algorithm");
    PlayerFrameSelectionPlan plan;
    plan.settings = parse_settings(node["settings"]);
    sequence_size(node["sources"], kMaximumSources);
    for (const auto& binding : node["sources"]) {
      keys(binding, {"path", "size", "modification_time_ns"});
      const std::string time = string_value(binding["modification_time_ns"], 20);
      size_t used = 0;
      const int64_t timestamp = std::stoll(time, &used);
      const size_t digit_start = !time.empty() && time.front() == '-' ? 1 : 0;
      if (used != time.size() || time.size() == digit_start ||
          time.find_first_not_of("0123456789", digit_start) != std::string::npos)
        return invalid("invalid source modification time");
      plan.sources.push_back({string_value(binding["path"], 4096), uint_value(binding["size"]), timestamp});
    }
    if (!node["context"].IsMap() || node["context"].size() > 64)
      return invalid("invalid context map");
    for (const auto& entry : node["context"]) {
      if (!plan.context.emplace(string_value(entry.first, 128), string_value(entry.second)).second)
        return invalid("duplicate context entry");
    }
    sequence_size(node["selected"], kPlayerFrameMaximumPairs);
    for (const auto& observation : node["selected"])
      plan.selected.push_back(parse_observation(observation));
    plan.fingerprint = string_value(node["fingerprint"], 64);
    auto status = ValidatePlayerFrameSelectionPlan(plan);
    if (!status.ok())
      return status;
    return plan;
  } catch (const std::exception& error) {
    return invalid(std::string("invalid plan YAML: ") + error.what());
  }
}

YAML::Node PlayerFrameSelectionReportYaml(const PlayerFrameSelectionReport& report) {
  YAML::Node node;
  node["schema"] = 1;
  node["observation_count"] = report.observation_count;
  for (auto count : report.observed_size_band_counts)
    node["observed_size_band_counts"].push_back(count);
  node["unavailable_reason"] = report.unavailable_reason;
  node["plan"] = report.plan ? PlayerFrameSelectionPlanYaml(*report.plan) : YAML::Node(YAML::NodeType::Null);
  return node;
}

absl::StatusOr<PlayerFrameSelectionReport> ParsePlayerFrameSelectionReport(const YAML::Node& node) {
  try {
    keys(node, {"schema", "observation_count", "observed_size_band_counts", "unavailable_reason", "plan"});
    if (uint_value(node["schema"]) != 1)
      return invalid("unsupported report schema");
    PlayerFrameSelectionReport report;
    report.observation_count = bounded_size(node["observation_count"], 1200);
    sequence_size(node["observed_size_band_counts"], 3);
    if (node["observed_size_band_counts"].size() != 3)
      return invalid("report requires three size bands");
    for (size_t band = 0; band < 3; ++band)
      report.observed_size_band_counts[band] =
          bounded_size(node["observed_size_band_counts"][band], report.observation_count * 256);
    if (report.observed_size_band_counts[0] + report.observed_size_band_counts[1] +
            report.observed_size_band_counts[2] >
        report.observation_count * 256)
      return invalid("report size-band totals exceed observation limits");
    report.unavailable_reason = string_value(node["unavailable_reason"], 4096);
    if (!node["plan"].IsNull()) {
      auto plan = ParsePlayerFrameSelectionPlan(node["plan"]);
      if (!plan.ok())
        return plan.status();
      if (report.observation_count < plan->selected.size() || !report.unavailable_reason.empty())
        return invalid("report plan/availability fields disagree");
      for (size_t band = 0; band < 3; ++band) {
        size_t selected_count = 0;
        for (const auto& observation : plan->selected)
          selected_count += observation.size_band_counts[band];
        if (selected_count > report.observed_size_band_counts[band])
          return invalid("selected counts exceed report observations");
      }
      report.plan = std::move(*plan);
    } else if (report.unavailable_reason.empty()) {
      return invalid("unavailable report needs a reason");
    }
    return report;
  } catch (const std::exception& error) {
    return invalid(std::string("invalid report YAML: ") + error.what());
  }
}

absl::StatusOr<PlayerFrameSelectionReport> LoadPlayerFrameSelectionReport(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size == 0 || size > kMaximumDocumentBytes)
    return invalid("missing, empty or oversized report");
  std::ifstream input(path, std::ios::binary);
  std::string bytes(static_cast<size_t>(size), '\0');
  if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
      input.peek() != std::char_traits<char>::eof())
    return invalid("report changed or could not be read");
  try {
    return ParsePlayerFrameSelectionReport(YAML::Load(bytes));
  } catch (const YAML::Exception& error) {
    return invalid(std::string("invalid report document: ") + error.what());
  }
}

absl::StatusOr<PlayerFrameReplaySelector> PlayerFrameReplaySelector::Create(const PlayerFrameSelectionPlan& plan) {
  auto status = ValidatePlayerFrameSelectionPlan(plan);
  if (!status.ok())
    return status;
  std::vector<PlayerFramePairIdentity> pairs;
  for (const auto& observation : plan.selected)
    pairs.push_back(observation.pair);
  return PlayerFrameReplaySelector(std::move(pairs));
}

absl::StatusOr<bool> PlayerFrameReplaySelector::Observe(const PlayerFramePairIdentity& actual) {
  auto status = validate_pair(actual);
  if (!status.ok())
    return status;
  if (complete())
    return false;
  if (previous_timeline_ns_ && actual.timeline_pts_ns <= *previous_timeline_ns_)
    return absl::FailedPreconditionError("Player-selection replay timeline is not strictly increasing");
  previous_timeline_ns_ = actual.timeline_pts_ns;
  const auto& expected = pairs_[next_];
  if (same_pair(actual, expected)) {
    if (actual.timeline_pts_ns != expected.timeline_pts_ns)
      return absl::FailedPreconditionError("Selected source pair has a different synchronized timeline");
    ++next_;
    return true;
  }
  if (next_ == 0)
    return absl::FailedPreconditionError("Player-selection replay did not begin at the exact scanned anchor pair");
  if (actual.timeline_pts_ns >= expected.timeline_pts_ns)
    return absl::FailedPreconditionError("Player-selection replay skipped or mismatched a selected source pair");
  return false;
}

absl::Status PlayerFrameReplaySelector::Finish() const {
  return complete() ? absl::OkStatus()
                    : absl::FailedPreconditionError("Input ended before every selected player frame was captured");
}

} // namespace hm::stitching
