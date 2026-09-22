#include "src/apps/hstream-ui/StitchingExperimentStore.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/PlayerFrameSelection.h"
#include "hstream/src/libs/stitching/TransactionState.h"

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaximumConfigBytes = 4 * 1024 * 1024;
constexpr size_t kMaximumFailureBytes = 4096;
constexpr size_t kMaximumOwnerBytes = 16 * 1024;

struct Reservation {
  uint64_t anchor_ns{0};
  std::string key;
  std::string token;
};

struct Index {
  StitchingExperimentCatalog catalog;
  std::map<int, Reservation> reservations;
};

struct Descriptor {
  int value{-1};
  ~Descriptor() {
    if (value >= 0)
      ::close(value);
  }
};

struct StoreLock {
  hm::stitching::PinnedDirectory directory;
  Descriptor descriptor;
  bool discard_pending{false};
};

absl::Status io_error(const std::string& action) {
  return absl::InternalError(action + ": " + std::strerror(errno));
}

void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::invalid_argument(message);
}

bool safe_component(const std::string& value) {
  return !value.empty() && value != "." && value != ".." && value.size() <= 255 &&
      value.find_first_of("/\\") == std::string::npos && value.find('\0') == std::string::npos;
}

std::string string_value(const YAML::Node& node, size_t maximum, bool allow_empty = false) {
  require(node.IsScalar(), "Experiment catalog value must be scalar");
  const auto value = node.as<std::string>();
  require(
      (allow_empty || !value.empty()) && value.size() <= maximum && value.find('\0') == std::string::npos,
      "Experiment catalog string exceeds its bounds");
  return value;
}

uint64_t uint_value(const YAML::Node& node) {
  const std::string value = string_value(node, 20);
  uint64_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  require(
      parsed.ec == std::errc() && parsed.ptr == value.data() + value.size(), "Experiment catalog integer is invalid");
  return result;
}

int count_value(const YAML::Node& node) {
  const uint64_t count = uint_value(node);
  require(count > 0 && count <= hm::stitching::kPlayerFrameMaximumPairs, "Experiment frame count is out of bounds");
  return static_cast<int>(count);
}

void keys(const YAML::Node& node, const std::set<std::string>& allowed) {
  require(node.IsMap(), "Experiment catalog entry must be a map");
  std::set<std::string> seen;
  for (const auto& item : node) {
    const auto key = string_value(item.first, 128);
    require(allowed.count(key) && seen.insert(key).second, "Unknown or duplicate experiment catalog field: " + key);
  }
}

std::string path_key(const StitchingExperimentStore& store, const fs::path& candidate) {
  require(candidate.is_absolute() && candidate == candidate.lexically_normal(), "Experiment path must be normalized");
  const auto relative = candidate.lexically_relative(store.directory);
  std::vector<std::string> components;
  for (const auto& component : relative)
    components.push_back(component.string());
  require(
      components.size() == 3 && components[0] == "sessions" && safe_component(components[1]) &&
          safe_component(components[2]),
      "Experiment record must be inside sessions/<session>/<candidate>");
  return relative.generic_string();
}

fs::path key_path(const StitchingExperimentStore& store, const std::string& key) {
  const fs::path path = store.directory / key;
  require(path_key(store, path) == key, "Experiment catalog contains an unsafe record path");
  return path;
}

uint64_t anchor(const StitchingExperimentSettings& settings) {
  return hm::stitch_frame_time_to_nanoseconds(settings.stitch_frame_time);
}

bool same_settings(const StitchingExperimentSettings& left, const StitchingExperimentSettings& right) {
  return left.control_points == right.control_points && left.frame_count == right.frame_count &&
      left.stitch_frame_time == right.stitch_frame_time && left.rink_rotation_degrees == right.rink_rotation_degrees;
}

YAML::Node settings_yaml(const StitchingExperimentSettings& settings) {
  require(
      settings.control_points > 0 && settings.frame_count > 0 &&
          settings.frame_count <= static_cast<int>(hm::stitching::kPlayerFrameMaximumPairs),
      "Experiment settings counts are invalid");
  (void)anchor(settings);
  YAML::Node node;
  node["control_points"] = settings.control_points;
  node["frame_count"] = settings.frame_count;
  node["reference"] = settings.stitch_frame_time;
  if (settings.rink_rotation_degrees) {
    for (double value : *settings.rink_rotation_degrees) {
      require(std::isfinite(value) && std::abs(value) <= 180, "Experiment rotation is invalid");
      node["rotation"].push_back(value);
    }
  }
  return node;
}

StitchingExperimentSettings parse_settings(const YAML::Node& node) {
  keys(node, {"control_points", "frame_count", "reference", "rotation"});
  StitchingExperimentSettings settings;
  const uint64_t control_points = uint_value(node["control_points"]);
  require(control_points > 0 && control_points <= std::numeric_limits<int>::max(), "Invalid control-point budget");
  settings.control_points = static_cast<int>(control_points);
  settings.frame_count = count_value(node["frame_count"]);
  settings.stitch_frame_time = string_value(node["reference"], 12);
  if (node["rotation"]) {
    require(node["rotation"].IsSequence() && node["rotation"].size() == 3, "Invalid experiment rotation tuple");
    std::array<double, 3> rotation;
    for (size_t index = 0; index < rotation.size(); ++index)
      rotation[index] = node["rotation"][index].as<double>();
    settings.rink_rotation_degrees = rotation;
  }
  (void)settings_yaml(settings);
  return settings;
}

void validate_record(const StitchingExperimentStore& store, const StoredStitchingExperiment& record) {
  (void)path_key(store, record.workspace.game_directory);
  require(
      record.workspace.root == record.workspace.game_directory.parent_path() &&
          record.workspace.game_id == record.workspace.game_directory.filename().string(),
      "Experiment workspace identity does not match its path");
  require(safe_component(record.workspace.invalidation_id), "Experiment workspace owner is invalid");
  const std::set<std::string> states{"queued", "running", "scan", "frozen", "complete", "failed", "quarantined"};
  require(states.count(record.state), "Unknown saved experiment state");
  require(
      record.selection_fingerprint.empty() ||
          (record.selection_fingerprint.size() == 64 &&
           record.selection_fingerprint.find_first_not_of("0123456789abcdef") == std::string::npos),
      "Invalid saved selection fingerprint");
  require(
      record.artifact_generation_id.size() <= 255 && record.artifact_generation_id.find('\0') == std::string::npos,
      "Invalid saved artifact generation");
  require(
      record.process_session_id >= 0 && record.process_session_id <= std::numeric_limits<int>::max(),
      "Invalid experiment process session");
  require(
      record.process_token.size() <= 160 && record.process_token.find('\0') == std::string::npos,
      "Invalid experiment process token");
  require(
      record.reservation_token.size() <= 160 && record.reservation_token.find('\0') == std::string::npos,
      "Invalid experiment reservation token");
  require(
      record.sequence > 0 && record.baseline_sequence >= 0 && record.selection_owner_sequence >= 0,
      "Invalid session-local experiment sequence");
  require(
      (record.baseline_sequence == 0) == record.baseline_workspace_key.empty() &&
          (record.selection_owner_sequence == 0) == record.selection_owner_workspace_key.empty(),
      "Experiment dependency sequences require their workspace keys");
  for (const auto& dependency : {record.baseline_workspace_key, record.selection_owner_workspace_key}) {
    if (!dependency.empty())
      (void)key_path(store, dependency);
  }
  require(
      record.scan_duration_seconds > 0 && record.scan_duration_seconds <= 300,
      "Experiment scan duration exceeds its bound");
  require(
      record.saved_selection_fingerprint.empty() ||
          (record.saved_selection_fingerprint.size() == 64 &&
           record.saved_selection_fingerprint.find_first_not_of("0123456789abcdef") == std::string::npos),
      "Invalid inherited experiment selection fingerprint");
  require(
      (record.state != "running" && record.state != "scan") || !record.process_token.empty(),
      "Running experiments require a persisted process token");
  require(
      record.state != "complete" || !record.artifact_generation_id.empty(),
      "Completed experiments require their artifact generation");
  require(
      record.failure.size() <= kMaximumFailureBytes && record.failure.find('\0') == std::string::npos,
      "Experiment failure message exceeds its limit");
  (void)settings_yaml(record.workspace.settings);
}

YAML::Node record_yaml(const StitchingExperimentStore& store, const StoredStitchingExperiment& record) {
  validate_record(store, record);
  YAML::Node node;
  node["key"] = path_key(store, record.workspace.game_directory);
  node["owner"] = record.workspace.invalidation_id;
  node["settings"] = settings_yaml(record.workspace.settings);
  node["state"] = record.state;
  node["selection"] = record.selection_fingerprint;
  node["artifact_generation"] = record.artifact_generation_id;
  node["process_session"] = record.process_session_id;
  node["process_token"] = record.process_token;
  node["reservation_token"] = record.reservation_token;
  node["sequence"] = record.sequence;
  node["baseline_sequence"] = record.baseline_sequence;
  node["selection_owner_sequence"] = record.selection_owner_sequence;
  node["baseline_workspace_key"] = record.baseline_workspace_key;
  node["selection_owner_workspace_key"] = record.selection_owner_workspace_key;
  node["saved_selection"] = record.saved_selection_fingerprint;
  node["scan_duration_seconds"] = record.scan_duration_seconds;
  node["requires_ice_mask"] = record.requires_ice_mask;
  node["failure"] = record.failure;
  node["revision"] = record.revision;
  return node;
}

StoredStitchingExperiment parse_record(const StitchingExperimentStore& store, const YAML::Node& node) {
  keys(
      node,
      {"key",
       "owner",
       "settings",
       "state",
       "selection",
       "artifact_generation",
       "process_session",
       "process_token",
       "reservation_token",
       "sequence",
       "baseline_sequence",
       "selection_owner_sequence",
       "baseline_workspace_key",
       "selection_owner_workspace_key",
       "saved_selection",
       "scan_duration_seconds",
       "requires_ice_mask",
       "failure",
       "revision"});
  StoredStitchingExperiment record;
  record.workspace.game_directory = key_path(store, string_value(node["key"], 520));
  record.workspace.root = record.workspace.game_directory.parent_path();
  record.workspace.game_id = record.workspace.game_directory.filename().string();
  record.workspace.invalidation_id = string_value(node["owner"], 255);
  record.workspace.settings = parse_settings(node["settings"]);
  record.state = string_value(node["state"], 32);
  record.selection_fingerprint = string_value(node["selection"], 64, true);
  record.artifact_generation_id = string_value(node["artifact_generation"], 255, true);
  const uint64_t session = uint_value(node["process_session"]);
  require(session <= std::numeric_limits<int>::max(), "Experiment process session overflows");
  record.process_session_id = static_cast<int64_t>(session);
  record.process_token = string_value(node["process_token"], 160, true);
  record.reservation_token = string_value(node["reservation_token"], 160, true);
  const auto sequence = [&](const char* key) {
    const uint64_t value = uint_value(node[key]);
    require(value <= std::numeric_limits<int>::max(), "Experiment queue sequence overflows");
    return static_cast<int>(value);
  };
  record.sequence = sequence("sequence");
  record.baseline_sequence = sequence("baseline_sequence");
  record.selection_owner_sequence = sequence("selection_owner_sequence");
  record.baseline_workspace_key = string_value(node["baseline_workspace_key"], 520, true);
  record.selection_owner_workspace_key = string_value(node["selection_owner_workspace_key"], 520, true);
  record.saved_selection_fingerprint = string_value(node["saved_selection"], 64, true);
  record.scan_duration_seconds = sequence("scan_duration_seconds");
  require(node["requires_ice_mask"].IsScalar(), "Experiment mask flag is invalid");
  record.requires_ice_mask = node["requires_ice_mask"].as<bool>();
  record.failure = string_value(node["failure"], kMaximumFailureBytes, true);
  record.revision = uint_value(node["revision"]);
  require(record.revision > 0, "Saved experiment revision must be positive");
  validate_record(store, record);
  return record;
}

absl::StatusOr<hm::stitching::PinnedDirectory> open_workspace(
    const StoreLock& lock,
    const StitchingExperimentStore& store,
    const fs::path& path) {
  const auto key = fs::path(path_key(store, path));
  auto current = hm::stitching::PinnedDirectory::Open(lock.directory.path(), "experiment store");
  if (!current.ok())
    return current.status();
  for (const auto& component : key) {
    auto next = current->OpenChild(component.string(), "experiment workspace");
    if (!next.ok())
      return next.status();
    if (!next->has_value())
      return absl::NotFoundError("Saved experiment workspace is missing: " + path.string());
    current = std::move(**next);
  }
  return std::move(*current);
}

absl::StatusOr<YAML::Node> read_config(const fs::path& path) {
  std::string text;
  HM_ASSIGN_OR_RETURN(
      text, hm::stitching::read_bounded_regular_file_no_follow(path, kMaximumConfigBytes, "experiment config"));
  auto node = YAML::Load(text);
  require(node.IsMap(), "Experiment config must contain a map");
  return node;
}

absl::StatusOr<YAML::Node> validate_workspace_config(
    const StoreLock& lock,
    const StitchingExperimentStore& store,
    const StitchingExperimentWorkspace& workspace) {
  auto directory = open_workspace(lock, store, workspace.game_directory);
  if (!directory.ok())
    return directory.status();
  YAML::Node config;
  HM_ASSIGN_OR_RETURN(config, read_config(directory->path() / "config.yaml"));
  const YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
  require(
      calibration.IsMap() && calibration["invalidation_id"].as<std::string>("") == workspace.invalidation_id,
      "Saved experiment workspace owner changed");
  require(
      calibration["control_points"].as<int>(0) == workspace.settings.control_points &&
          calibration["frame_count"].as<int>(0) == workspace.settings.frame_count &&
          config["stitching"]["calibration_frame_count"].as<int>(0) == workspace.settings.frame_count,
      "Saved experiment count/settings no longer match their workspace");
  return config;
}

absl::StatusOr<hm::stitching::PlayerFrameSelectionPlan> selected_plan(const YAML::Node& config) {
  return hm::stitching::ParsePlayerFrameSelectionPlan(config["stitching"]["calibration_frame_selection"]);
}

uint64_t plan_anchor(const hm::stitching::PlayerFrameSelectionPlan& plan) {
  const auto found = plan.context.find("decode_anchor_ns");
  require(found != plan.context.end(), "Saved selection has no numeric decode anchor");
  return uint_value(YAML::Node(found->second));
}

absl::Status make_private_directory(const fs::path& path);

absl::StatusOr<std::unique_ptr<StoreLock>> acquire(
    const StitchingExperimentStore& store,
    bool validate_owner = true,
    bool create = false,
    bool allow_pending_discard = false) {
  require(
      store.directory.is_absolute() && store.directory == store.directory.lexically_normal() &&
          store.game_directory.is_absolute() && store.game_directory == store.game_directory.lexically_normal(),
      "Experiment store paths must be canonical absolute paths");
  require(store.directory == store.game_directory / "stitching-experiments", "Experiment store is outside its game");
  auto game = hm::stitching::PinnedDirectory::Open(store.game_directory, "experiment game");
  if (!game.ok())
    return game.status();
  auto lock = std::make_unique<StoreLock>();
  // This inode survives explicit cache deletion. A lock inside the removable
  // directory would let a concurrent opener create a second lock during discard.
  lock->descriptor.value = ::openat(
      game->descriptor(), ".stitching-experiments.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
  if (lock->descriptor.value < 0)
    return io_error("Cannot open experiment store lock");
  struct stat metadata{};
  if (::fstat(lock->descriptor.value, &metadata) != 0 || !S_ISREG(metadata.st_mode))
    return absl::FailedPreconditionError("Experiment store lock is not a regular file");
  while (::flock(lock->descriptor.value, LOCK_EX) != 0) {
    if (errno != EINTR)
      return io_error("Cannot lock experiment store");
  }
  if (create)
    HM_RETURN_IF_ERROR(make_private_directory(game->path() / "stitching-experiments"));
  auto directory = game->OpenChild("stitching-experiments", "experiment store");
  if (!directory.ok())
    return directory.status();
  if (!directory->has_value())
    return absl::NotFoundError("Experiment store was removed");
  lock->directory = std::move(**directory);
  if (validate_owner) {
    std::string text;
    HM_ASSIGN_OR_RETURN(
        text,
        hm::stitching::read_bounded_regular_file_no_follow(
            lock->directory.path() / "owner.yaml", kMaximumOwnerBytes, "experiment store owner"));
    const auto owner = YAML::Load(text);
    keys(owner, {"version", "game_directory", "discard_pending"});
    require(
        uint_value(owner["version"]) == 1 &&
            string_value(owner["game_directory"], kMaximumOwnerBytes) == store.game_directory.generic_string(),
        "Experiment store belongs to another game or schema");
    lock->discard_pending = owner["discard_pending"].as<bool>(false);
    if (lock->discard_pending && !allow_pending_discard)
      return absl::FailedPreconditionError(
          "Experiment discard was interrupted. Use Discard results again to finish removing its retained data.");
  }
  return lock;
}

absl::Status atomic_write(const StoreLock& lock, const std::string& name, const std::string& contents) {
  static std::atomic<uint64_t> sequence{0};
  std::string temporary;
  Descriptor output;
  for (int attempt = 0; attempt < 16; ++attempt) {
    temporary = ".write-" + std::to_string(::getpid()) + "-" + std::to_string(++sequence);
    output.value = ::openat(
        lock.directory.descriptor(), temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (output.value >= 0)
      break;
    if (errno != EEXIST)
      return io_error("Cannot stage experiment catalog");
  }
  if (output.value < 0)
    return absl::AlreadyExistsError("Cannot reserve an experiment catalog staging file");
  struct Cleanup {
    int directory;
    const std::string& name;
    ~Cleanup() {
      ::unlinkat(directory, name.c_str(), 0);
    }
  } cleanup{lock.directory.descriptor(), temporary};
  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t written = ::write(output.value, contents.data() + offset, contents.size() - offset);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      return io_error("Cannot write experiment catalog");
    offset += written;
  }
  if (::fsync(output.value) != 0)
    return io_error("Cannot sync experiment catalog");
  if (::renameat(lock.directory.descriptor(), temporary.c_str(), lock.directory.descriptor(), name.c_str()) != 0)
    return io_error("Cannot publish experiment catalog");
  return ::fsync(lock.directory.descriptor()) == 0 ? absl::OkStatus() : io_error("Cannot sync experiment store");
}

absl::Status write_index(const StoreLock& lock, const StitchingExperimentStore& store, const Index& index) {
  if (index.catalog.experiments.size() > kMaximumStoredStitchingExperiments)
    return absl::ResourceExhaustedError("Experiment history is full; retained frame sets were not removed");
  YAML::Node node;
  node["version"] = 1;
  node["experiments"] = YAML::Node(YAML::NodeType::Sequence);
  node["selected_by_count"] = YAML::Node(YAML::NodeType::Map);
  node["reservations"] = YAML::Node(YAML::NodeType::Map);
  for (const auto& record : index.catalog.experiments)
    node["experiments"].push_back(record_yaml(store, record));
  for (const auto& [count, key] : index.catalog.selected_by_count)
    node["selected_by_count"][std::to_string(count)] = key;
  for (const auto& [count, reservation] : index.reservations) {
    auto item = node["reservations"][std::to_string(count)];
    item["anchor_ns"] = reservation.anchor_ns;
    item["key"] = reservation.key;
    item["token"] = reservation.token;
  }
  YAML::Emitter emitter;
  emitter.SetDoublePrecision(std::numeric_limits<double>::max_digits10);
  emitter << node;
  if (!emitter.good() || emitter.size() + 1 > kMaximumStitchingExperimentCatalogBytes)
    return absl::ResourceExhaustedError("Experiment catalog is full; retained history was not truncated");
  return atomic_write(lock, "index.yaml", std::string(emitter.c_str()) + '\n');
}

absl::StatusOr<Index> read_index(const StoreLock& lock, const StitchingExperimentStore& store) {
  std::string text;
  HM_ASSIGN_OR_RETURN(
      text,
      hm::stitching::read_bounded_regular_file_no_follow(
          lock.directory.path() / "index.yaml", kMaximumStitchingExperimentCatalogBytes, "experiment catalog"));
  const auto node = YAML::Load(text);
  keys(node, {"version", "experiments", "selected_by_count", "reservations"});
  require(uint_value(node["version"]) == 1, "Unknown experiment catalog version");
  require(
      node["experiments"].IsSequence() && node["experiments"].size() <= kMaximumStoredStitchingExperiments,
      "Experiment history exceeds its row limit");
  Index index;
  std::set<std::string> record_keys;
  std::set<std::pair<fs::path, int>> session_sequences;
  for (const auto& item : node["experiments"]) {
    auto record = parse_record(store, item);
    const auto key = path_key(store, record.workspace.game_directory);
    require(record_keys.insert(key).second, "Duplicate saved experiment record");
    require(
        session_sequences.emplace(record.workspace.root, record.sequence).second,
        "Duplicate experiment sequence within one session");
    auto directory = open_workspace(lock, store, record.workspace.game_directory);
    if (!directory.ok())
      return directory.status();
    index.catalog.experiments.push_back(std::move(record));
  }
  for (const auto& record : index.catalog.experiments) {
    for (const auto& dependency : {record.baseline_workspace_key, record.selection_owner_workspace_key})
      require(dependency.empty() || record_keys.count(dependency), "Experiment dependency refers to a missing record");
  }
  require(
      node["selected_by_count"].IsMap() && node["selected_by_count"].size() <= hm::stitching::kPlayerFrameMaximumPairs,
      "Invalid selected-count catalog");
  for (const auto& item : node["selected_by_count"]) {
    const int count = count_value(item.first);
    const auto key = string_value(item.second, 520);
    require(
        record_keys.count(key) && index.catalog.selected_by_count.emplace(count, key).second,
        "Selected count refers to a missing or duplicate record");
    const auto found =
        std::find_if(index.catalog.experiments.begin(), index.catalog.experiments.end(), [&](const auto& record) {
          return path_key(store, record.workspace.game_directory) == key;
        });
    require(
        found->workspace.settings.frame_count == count && !found->selection_fingerprint.empty(),
        "Selected-count owner has no matching frozen plan");
  }
  require(
      node["reservations"].IsMap() && node["reservations"].size() <= hm::stitching::kPlayerFrameMaximumPairs,
      "Invalid count reservation catalog");
  for (const auto& item : node["reservations"]) {
    const int count = count_value(item.first);
    keys(item.second, {"anchor_ns", "key", "token"});
    Reservation reservation{
        uint_value(item.second["anchor_ns"]),
        string_value(item.second["key"], 520),
        string_value(item.second["token"], 160)};
    (void)key_path(store, reservation.key);
    require(index.reservations.emplace(count, std::move(reservation)).second, "Duplicate count reservation");
  }
  for (auto row = index.catalog.experiments.rbegin(); row != index.catalog.experiments.rend(); ++row) {
    const int count = row->workspace.settings.frame_count;
    if (!row->saved_selection_fingerprint.empty() && row->saved_selection_fingerprint == row->selection_fingerprint &&
        !index.catalog.selected_by_count.count(count) && !index.reservations.count(count)) {
      // Insertion order records when each immutable main snapshot was queued;
      // later runner state revisions must not make an older snapshot current.
      index.catalog.retained_by_count.emplace(count, path_key(store, row->workspace.game_directory));
    }
  }
  return index;
}

absl::Status make_private_directory(const fs::path& path) {
  if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
    return io_error("Cannot create experiment directory");
  struct stat metadata{};
  if (::lstat(path.c_str(), &metadata) != 0)
    return io_error("Cannot inspect experiment directory");
  if (!S_ISDIR(metadata.st_mode))
    return absl::FailedPreconditionError("Experiment directory is not a real directory: " + path.string());
  Descriptor parent{::open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY)};
  if (parent.value < 0 || ::fsync(parent.value) != 0)
    return io_error("Cannot sync experiment directory creation");
  return absl::OkStatus();
}

absl::Status invalid_catalog(const std::exception& error) {
  return absl::InvalidArgumentError("Invalid saved stitching experiments: " + std::string(error.what()));
}

} // namespace

absl::StatusOr<StitchingExperimentStore> OpenStitchingExperimentStore(const fs::path& game_directory) {
  try {
    const auto game = fs::canonical(game_directory);
    require(fs::is_directory(game), "Experiment game is not a directory");
    const fs::path directory = game / "stitching-experiments";
    StitchingExperimentStore store{directory, game};
    auto lock = acquire(store, false, true);
    if (!lock.ok())
      return lock.status();
    struct stat owner{};
    if (::fstatat((*lock)->directory.descriptor(), "owner.yaml", &owner, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno != ENOENT)
        return io_error("Cannot inspect experiment owner");
      // Never adopt an existing unrelated or interrupted directory as empty.
      for (const auto& entry : fs::directory_iterator((*lock)->directory.path()))
        require(entry.path().filename() == ".lock", "Experiment store has data but no valid ownership marker");
      HM_RETURN_IF_ERROR(write_index(**lock, store, {}));
      YAML::Node marker;
      marker["version"] = 1;
      marker["game_directory"] = game.generic_string();
      const std::string contents = YAML::Dump(marker) + '\n';
      require(contents.size() <= kMaximumOwnerBytes, "Canonical game path exceeds the owner-marker limit");
      HM_RETURN_IF_ERROR(atomic_write(**lock, "owner.yaml", contents));
    } else {
      std::string text;
      HM_ASSIGN_OR_RETURN(
          text,
          hm::stitching::read_bounded_regular_file_no_follow(
              (*lock)->directory.path() / "owner.yaml", kMaximumOwnerBytes, "experiment store owner"));
      const auto marker = YAML::Load(text);
      keys(marker, {"version", "game_directory", "discard_pending"});
      require(
          uint_value(marker["version"]) == 1 &&
              string_value(marker["game_directory"], kMaximumOwnerBytes) == game.generic_string(),
          "Experiment store belongs to another game or schema");
      store.discard_pending = marker["discard_pending"].as<bool>(false);
      if (store.discard_pending)
        return store;
      auto index = read_index(**lock, store);
      if (!index.ok())
        return index.status();
    }
    HM_RETURN_IF_ERROR(make_private_directory(directory / "sessions"));
    return store;
  } catch (const std::exception& error) {
    return invalid_catalog(error);
  }
}

absl::StatusOr<StitchingExperimentCatalog> LoadStitchingExperimentStore(const StitchingExperimentStore& store) {
  try {
    auto lock = acquire(store);
    if (!lock.ok())
      return lock.status();
    Index index;
    HM_ASSIGN_OR_RETURN(index, read_index(**lock, store));
    return std::move(index.catalog);
  } catch (const std::exception& error) {
    return invalid_catalog(error);
  }
}

absl::Status MarkStitchingExperimentProcessStopped(
    const StitchingExperimentStore& store,
    StoredStitchingExperiment& experiment,
    const std::string& failure) {
  try {
    auto lock = acquire(store);
    if (!lock.ok())
      return lock.status();
    Index index;
    HM_ASSIGN_OR_RETURN(index, read_index(**lock, store));
    const auto found =
        std::find_if(index.catalog.experiments.begin(), index.catalog.experiments.end(), [&](const auto& record) {
          return record.workspace.game_directory == experiment.workspace.game_directory;
        });
    if (found == index.catalog.experiments.end() || found->revision != experiment.revision ||
        found->revision == std::numeric_limits<uint64_t>::max() ||
        found->workspace.invalidation_id != experiment.workspace.invalidation_id ||
        found->process_session_id != experiment.process_session_id || found->process_token != experiment.process_token)
      return absl::AbortedError("Saved experiment process ownership changed before shutdown reconciliation");
    // Process death is independent of calibration validity. Never require a
    // readable config merely to make an explicitly discarded cache removable.
    found->state = found->state == "complete" ? "complete" : "failed";
    found->process_session_id = 0;
    found->process_token.clear();
    found->failure = failure;
    ++found->revision;
    validate_record(store, *found);
    HM_RETURN_IF_ERROR(write_index(**lock, store, index));
    experiment = *found;
    return absl::OkStatus();
  } catch (const std::exception& error) {
    return invalid_catalog(error);
  }
}

absl::Status SaveStitchingExperiment(
    const StitchingExperimentStore& store,
    StoredStitchingExperiment& experiment,
    bool publish_selection,
    const std::string& authoritative_main_fingerprint) {
  try {
    validate_record(store, experiment);
    auto lock = acquire(store);
    if (!lock.ok())
      return lock.status();
    Index index;
    HM_ASSIGN_OR_RETURN(index, read_index(**lock, store));
    YAML::Node config;
    HM_ASSIGN_OR_RETURN(config, validate_workspace_config(**lock, store, experiment.workspace));
    if (!experiment.selection_fingerprint.empty()) {
      hm::stitching::PlayerFrameSelectionPlan plan;
      HM_ASSIGN_OR_RETURN(plan, selected_plan(config));
      require(
          plan.fingerprint == experiment.selection_fingerprint &&
              plan.selected.size() == static_cast<size_t>(experiment.workspace.settings.frame_count) &&
              plan_anchor(plan) == anchor(experiment.workspace.settings),
          "Saved experiment selection does not match its owned config");
    }
    const auto key = path_key(store, experiment.workspace.game_directory);
    for (const auto& dependency : {experiment.baseline_workspace_key, experiment.selection_owner_workspace_key}) {
      require(
          dependency.empty() || dependency == key ||
              std::any_of(
                  index.catalog.experiments.begin(),
                  index.catalog.experiments.end(),
                  [&](const auto& record) { return path_key(store, record.workspace.game_directory) == dependency; }),
          "Experiment dependency was not durably saved");
    }
    auto found =
        std::find_if(index.catalog.experiments.begin(), index.catalog.experiments.end(), [&](const auto& value) {
          return value.workspace.game_directory == experiment.workspace.game_directory;
        });
    const uint64_t previous = found == index.catalog.experiments.end() ? 0 : found->revision;
    if (previous != experiment.revision || previous == std::numeric_limits<uint64_t>::max())
      return absl::AbortedError("Saved experiment changed in another dialog; reload it before updating");
    if (found != index.catalog.experiments.end()) {
      require(
          found->workspace.invalidation_id == experiment.workspace.invalidation_id &&
              same_settings(found->workspace.settings, experiment.workspace.settings) &&
              found->sequence == experiment.sequence && found->baseline_sequence == experiment.baseline_sequence &&
              found->selection_owner_sequence == experiment.selection_owner_sequence &&
              found->baseline_workspace_key == experiment.baseline_workspace_key &&
              found->selection_owner_workspace_key == experiment.selection_owner_workspace_key &&
              found->saved_selection_fingerprint == experiment.saved_selection_fingerprint &&
              found->scan_duration_seconds == experiment.scan_duration_seconds,
          "An experiment row cannot change its owner or solve settings");
      require(
          found->selection_fingerprint.empty() || found->selection_fingerprint == experiment.selection_fingerprint,
          "A frozen experiment row cannot replace or remove its selected frames");
    }
    bool main_authority = false;
    if (publish_selection) {
      require(
          !experiment.selection_fingerprint.empty() && experiment.state != "running" && experiment.state != "scan" &&
              experiment.state != "quarantined" && experiment.state != "queued",
          "Only a stopped frozen selection may become a count's owner");
      if (!authoritative_main_fingerprint.empty()) {
        require(
            authoritative_main_fingerprint == experiment.selection_fingerprint,
            "The requested main authority does not match the experiment selection");
        // A queued solve owns its frozen inputs even if main changes before it
        // finishes. A different main count leaves this count available, but a
        // newer same-count default/reservation must never be overwritten.
        publish_selection = false;
        const auto main = read_config(store.game_directory / "config.yaml");
        if (main.ok()) {
          const auto plan = selected_plan(*main);
          const int count = experiment.workspace.settings.frame_count;
          const auto latest = std::find_if(
              index.catalog.experiments.rbegin(), index.catalog.experiments.rend(), [&](const auto& record) {
                return record.workspace.settings.frame_count == count && !record.saved_selection_fingerprint.empty() &&
                    record.saved_selection_fingerprint == record.selection_fingerprint;
              });
          const bool latest_snapshot = found == index.catalog.experiments.end() ||
              latest == index.catalog.experiments.rend() ||
              latest->selection_fingerprint == experiment.selection_fingerprint;
          main_authority = plan.ok() && plan->fingerprint == authoritative_main_fingerprint &&
              plan->selected.size() == static_cast<size_t>(count);
          publish_selection = main_authority ||
              (plan.ok() && plan->selected.size() != static_cast<size_t>(count) && latest_snapshot &&
               !index.catalog.selected_by_count.count(count) && !index.reservations.count(count));
        }
      }
    }
    if (publish_selection) {
      const int count = experiment.workspace.settings.frame_count;
      const auto selected = index.catalog.selected_by_count.find(count);
      if (selected != index.catalog.selected_by_count.end()) {
        const auto old =
            std::find_if(index.catalog.experiments.begin(), index.catalog.experiments.end(), [&](const auto& value) {
              return path_key(store, value.workspace.game_directory) == selected->second;
            });
        if (old->selection_fingerprint != experiment.selection_fingerprint && !main_authority)
          return absl::FailedPreconditionError("This frame count already has a different saved selection");
      }
      const auto reserved = index.reservations.find(count);
      if (reserved != index.reservations.end()) {
        if (reserved->second.key != key && !main_authority)
          return absl::FailedPreconditionError("Another experiment owns this count's unfinished selection");
        if (reserved->second.key == key) {
          require(
              reserved->second.anchor_ns == anchor(experiment.workspace.settings) &&
                  reserved->second.token == experiment.reservation_token,
              "Reserved selection reference or owner token changed");
          index.reservations.erase(reserved);
        }
      }
      index.catalog.selected_by_count[count] = key;
    }
    auto updated = experiment;
    updated.revision = previous + 1;
    if (found == index.catalog.experiments.end()) {
      if (index.catalog.experiments.size() >= kMaximumStoredStitchingExperiments)
        return absl::ResourceExhaustedError("Experiment history is full; no retained rows were removed");
      for (const auto& other : index.catalog.experiments) {
        require(
            other.workspace.root != experiment.workspace.root || other.sequence != experiment.sequence,
            "Another experiment already owns this session sequence");
      }
      index.catalog.experiments.push_back(updated);
    } else {
      *found = updated;
    }
    HM_RETURN_IF_ERROR(write_index(**lock, store, index));
    experiment.revision = updated.revision;
    return absl::OkStatus();
  } catch (const std::exception& error) {
    return invalid_catalog(error);
  }
}

absl::StatusOr<std::optional<StoredStitchingExperiment>> ReserveStitchingExperimentFrameCount(
    const StitchingExperimentStore& store,
    int frame_count,
    uint64_t anchor_ns,
    const StitchingExperimentWorkspace& owner_workspace,
    const std::string& reservation_token) {
  try {
    require(
        frame_count > 0 && frame_count <= static_cast<int>(hm::stitching::kPlayerFrameMaximumPairs) &&
            owner_workspace.settings.frame_count == frame_count && anchor(owner_workspace.settings) == anchor_ns,
        "Count reservation does not match its workspace/reference");
    require(
        !reservation_token.empty() && reservation_token.size() <= 160 &&
            reservation_token.find('\0') == std::string::npos,
        "Invalid count reservation token");
    auto lock = acquire(store);
    if (!lock.ok())
      return lock.status();
    Index index;
    HM_ASSIGN_OR_RETURN(index, read_index(**lock, store));
    auto config = validate_workspace_config(**lock, store, owner_workspace);
    if (!config.ok())
      return config.status();
    const auto selected = index.catalog.selected_by_count.find(frame_count);
    const auto retained = index.catalog.retained_by_count.find(frame_count);
    const std::string selected_key = selected != index.catalog.selected_by_count.end() ? selected->second
        : retained != index.catalog.retained_by_count.end()                            ? retained->second
                                                                                       : std::string();
    // Recheck at Start under the same lock as reservation creation. A dialog
    // may have queued its scan before another saved this count's exact inputs.
    // Existing reservations already suppress the derived retained fallback.
    if (!selected_key.empty()) {
      const auto found =
          std::find_if(index.catalog.experiments.begin(), index.catalog.experiments.end(), [&](const auto& value) {
            return path_key(store, value.workspace.game_directory) == selected_key;
          });
      if (found->state == "quarantined" || found->state == "running" || found->state == "scan")
        return absl::FailedPreconditionError("Saved selected-frame owner has unconfirmed process ownership");
      if (anchor(found->workspace.settings) != anchor_ns)
        return absl::FailedPreconditionError("This frame count already fixes a different reference time");
      auto owner_config = validate_workspace_config(**lock, store, found->workspace);
      if (!owner_config.ok())
        return owner_config.status();
      hm::stitching::PlayerFrameSelectionPlan plan;
      HM_ASSIGN_OR_RETURN(plan, selected_plan(*owner_config));
      require(
          plan.fingerprint == found->selection_fingerprint && plan_anchor(plan) == anchor_ns,
          "The cached count's frozen plan changed");
      return std::optional<StoredStitchingExperiment>(*found);
    }
    const std::string key = path_key(store, owner_workspace.game_directory);
    const auto existing = index.reservations.find(frame_count);
    if (existing != index.reservations.end()) {
      if (existing->second.anchor_ns != anchor_ns || existing->second.key != key ||
          existing->second.token != reservation_token)
        return absl::FailedPreconditionError("This frame count is reserved by another unfinished selection");
      return std::nullopt;
    }
    index.reservations[frame_count] = Reservation{anchor_ns, key, reservation_token};
    HM_RETURN_IF_ERROR(write_index(**lock, store, index));
    return std::nullopt;
  } catch (const std::exception& error) {
    return invalid_catalog(error);
  }
}

absl::Status ReleaseStitchingExperimentFrameCount(
    const StitchingExperimentStore& store,
    int frame_count,
    const StitchingExperimentWorkspace& owner_workspace,
    const std::string& reservation_token) {
  try {
    auto lock = acquire(store);
    if (!lock.ok())
      return lock.status();
    Index index;
    HM_ASSIGN_OR_RETURN(index, read_index(**lock, store));
    const auto found = index.reservations.find(frame_count);
    if (found == index.reservations.end())
      return absl::OkStatus();
    if (found->second.key != path_key(store, owner_workspace.game_directory) ||
        found->second.token != reservation_token)
      return absl::FailedPreconditionError("Cannot release another experiment's frame-count reservation");
    index.reservations.erase(found);
    return write_index(**lock, store, index);
  } catch (const std::exception& error) {
    return invalid_catalog(error);
  }
}

absl::Status DiscardStitchingExperimentStore(const StitchingExperimentStore& store) {
  try {
    auto lock = acquire(store, true, false, true);
    if (!lock.ok())
      return lock.status();
    if (!(*lock)->discard_pending) {
      Index index;
      HM_ASSIGN_OR_RETURN(index, read_index(**lock, store));
      for (const auto& record : index.catalog.experiments) {
        if (record.state == "running" || record.state == "scan" || record.state == "quarantined" ||
            record.process_session_id != 0 || !record.process_token.empty())
          return absl::FailedPreconditionError(
              "Confirm every experiment runner has stopped before discarding its retained data");
      }
      // Commit the explicit discard decision before any payload is removed.
      // The retained owner marker allows retries even after index.yaml or a
      // referenced workspace has gone; normal operations cannot revive it.
      YAML::Node marker;
      marker["version"] = 1;
      marker["game_directory"] = store.game_directory.generic_string();
      marker["discard_pending"] = true;
      HM_RETURN_IF_ERROR(atomic_write(**lock, "owner.yaml", YAML::Dump(marker) + '\n'));
    }
    auto parent = hm::stitching::PinnedDirectory::Open(store.game_directory, "experiment game");
    if (!parent.ok())
      return parent.status();
    const auto removed =
        hm::stitching::remove_pinned_directory(*parent, "stitching-experiments", (*lock)->directory, "owner.yaml");
    return removed.ok()
        ? removed
        : absl::InternalError(
              "Experiment discard started but file cleanup is incomplete. Use Discard results again to finish: " +
              removed.ToString());
  } catch (const std::exception& error) {
    return invalid_catalog(error);
  }
}

absl::Status RemoveQueuedStitchingExperiments(
    const StitchingExperimentStore& store,
    const std::vector<std::string>& workspace_keys) {
  try {
    require(
        !workspace_keys.empty() && workspace_keys.size() <= kMaximumStoredStitchingExperiments,
        "Queued removal requires a bounded set of rows");
    std::set<std::string> removed;
    for (const auto& key : workspace_keys) {
      (void)key_path(store, key);
      require(removed.insert(key).second, "Queued removal contains duplicate rows");
    }
    auto lock = acquire(store);
    if (!lock.ok())
      return lock.status();
    Index index;
    HM_ASSIGN_OR_RETURN(index, read_index(**lock, store));
    std::vector<StoredStitchingExperiment> records;
    for (const auto& record : index.catalog.experiments) {
      const auto key = path_key(store, record.workspace.game_directory);
      if (!removed.count(key))
        continue;
      require(
          record.state == "queued" && record.process_session_id == 0 && record.process_token.empty(),
          "Only stopped queued rows may be removed individually");
      auto config = validate_workspace_config(**lock, store, record.workspace);
      if (!config.ok())
        return config.status();
      records.push_back(record);
    }
    require(records.size() == removed.size(), "A queued row changed or disappeared before removal");
    for (const auto& [count, key] : index.catalog.selected_by_count) {
      (void)count;
      require(!removed.count(key), "Cannot remove the owner of a retained frame selection");
    }
    for (const auto& [count, reservation] : index.reservations) {
      (void)count;
      require(!removed.count(reservation.key), "Release the unfinished count reservation before removing its owner");
    }
    for (const auto& survivor : index.catalog.experiments) {
      if (removed.count(path_key(store, survivor.workspace.game_directory)))
        continue;
      require(
          !removed.count(survivor.baseline_workspace_key) && !removed.count(survivor.selection_owner_workspace_key),
          "Remove dependent queued rows before their input owner");
      for (const auto& record : records) {
        if (survivor.workspace.root != record.workspace.root)
          continue;
        require(
            survivor.baseline_sequence != record.sequence && survivor.selection_owner_sequence != record.sequence,
            "Remove dependent queued rows before their session-local input owner");
      }
    }
    struct Removal {
      hm::stitching::PinnedDirectory parent;
      hm::stitching::PinnedDirectory candidate;
      std::string name;
    };
    std::vector<Removal> removals;
    for (const auto& record : records) {
      auto candidate = open_workspace(**lock, store, record.workspace.game_directory);
      if (!candidate.ok())
        return candidate.status();
      auto parent = hm::stitching::PinnedDirectory::Open(record.workspace.root, "queued experiment session");
      if (!parent.ok())
        return parent.status();
      removals.push_back({std::move(*parent), std::move(*candidate), record.workspace.game_id});
    }
    auto& experiments = index.catalog.experiments;
    experiments.erase(
        std::remove_if(
            experiments.begin(),
            experiments.end(),
            [&](const auto& record) { return removed.count(path_key(store, record.workspace.game_directory)); }),
        experiments.end());
    HM_RETURN_IF_ERROR(write_index(**lock, store, index));
    for (const auto& removal : removals) {
      const auto status =
          hm::stitching::remove_pinned_directory(removal.parent, removal.name, removal.candidate, "config.yaml");
      if (!status.ok())
        return absl::InternalError(
            "Queued records were removed from history, but private files remain in " + store.directory.string() +
            ". Reload the saved rows; use Discard results or remove the leftover files manually: " + status.ToString());
    }
    return absl::OkStatus();
  } catch (const std::exception& error) {
    return invalid_catalog(error);
  }
}
