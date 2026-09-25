#include "hstream/src/libs/tracker_reid/ReIdConfig.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>
#include <utility>

#include "yaml-cpp/yaml.h"

namespace hm::tracker_reid {
namespace {
namespace fs = std::filesystem;
constexpr size_t kMaxConfigBytes = 1024 * 1024;

void require(bool valid, const std::string& message) {
  if (!valid)
    throw std::runtime_error(message);
}

YAML::Node load_config(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "Cannot read ReID configuration: " + path.string());
  std::string contents(kMaxConfigBytes + 1, '\0');
  input.read(contents.data(), contents.size());
  contents.resize(input.gcount());
  require(contents.size() <= kMaxConfigBytes, "ReID configuration exceeds 1 MiB");
  // NVIDIA's low-level examples use OpenCV's nonstandard YAML directive.
  if (contents.rfind("%YAML:1.0", 0) == 0)
    contents.erase(0, contents.find('\n') == std::string::npos ? contents.size() : contents.find('\n') + 1);
  YAML::Node result = YAML::Load(contents);
  require(result.IsMap(), "ReID configuration must be a YAML map: " + path.string());
  return result;
}

void validate_keys(const YAML::Node& node, const std::set<std::string>& allowed, const std::string& section) {
  require(node.IsMap(), section + " must be a map");
  std::set<std::string> seen;
  for (const auto& entry : node) {
    require(entry.first.IsScalar(), section + " keys must be strings");
    const std::string key = entry.first.as<std::string>();
    require(seen.insert(key).second, "Duplicate " + section + "." + key);
    require(allowed.count(key) != 0, "Unsupported " + section + "." + key);
  }
}

int integer(YAML::Node node, const char* key, int low, int high) {
  require(node[key] && node[key].IsScalar(), std::string("ReID requires ") + key);
  const int value = node[key].as<int>();
  require(value >= low && value <= high, std::string("ReID ") + key + " is out of range");
  return value;
}

double scalar(YAML::Node node, const char* key, double low, double high) {
  require(node[key] && node[key].IsScalar(), std::string("ReID requires ") + key);
  const double value = node[key].as<double>();
  require(std::isfinite(value) && value >= low && value <= high, std::string("ReID ") + key + " is out of range");
  return value;
}

std::string single_config(std::string value) {
  // The app's existing parser appends a delimiter even for one config.
  if (!value.empty() && value.back() == ';')
    value.pop_back();
  require(
      !value.empty() && value.find(';') == std::string::npos,
      "ReID requires one low-level tracker configuration; sub-batches are unsupported");
  return value;
}

void write_config(const fs::path& path, const std::string& contents) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  struct FileGuard {
    int descriptor;
    ~FileGuard() {
      if (descriptor >= 0)
        ::close(descriptor);
    }
  } file_guard{fd};
  require(fd >= 0, "Cannot create private ReID configuration: " + std::string(std::strerror(errno)));
  size_t written = 0;
  while (written < contents.size()) {
    const ssize_t count = ::write(fd, contents.data() + written, contents.size() - written);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      const int error = errno;
      throw std::runtime_error("Cannot write ReID configuration: " + std::string(std::strerror(error)));
    }
    written += static_cast<size_t>(count);
  }
  const bool synced = ::fsync(fd) == 0;
  const bool closed = ::close(fd) == 0;
  file_guard.descriptor = -1;
  require(synced && closed, "Cannot finish private ReID configuration");
}
} // namespace

PreparedConfig::PreparedConfig(fs::path directory)
    : directory_(std::move(directory)),
      path_(directory_ / "tracker.yaml"),
      temporary_path_(directory_ / "tracker.yaml.tmp") {}

PreparedConfig::~PreparedConfig() {
  std::error_code ignored;
  fs::remove(path_, ignored);
  fs::remove(temporary_path_, ignored);
  fs::remove(directory_, ignored);
}

absl::StatusOr<std::unique_ptr<PreparedConfig>> Prepare(const Options& options) {
  if (!options.tracker_enabled || !options.reid_enabled)
    return std::unique_ptr<PreparedConfig>{};
  try {
    require(options.sub_batches.empty(), "ReID does not support tracker sub-batches");
    require(
        fs::path(options.tracker_library).filename() == "libnvds_nvmultiobjecttracker.so",
        "ReID requires NVIDIA's native multiobject tracker library");
    require(!options.overlay_config.empty(), "reid-enable requires reid-config-file");
    const fs::path base_path = fs::absolute(single_config(options.base_config));
    const fs::path overlay_path = fs::absolute(options.overlay_config);
    YAML::Node base = load_config(base_path);
    YAML::Node overlay = load_config(overlay_path);
    validate_keys(overlay, {"ReID", "TrajectoryManagement"}, "ReID overlay");
    require(base["VisualTracker"].IsMap(), "ReID requires an NvDCF VisualTracker configuration");
    const int tracker_type = integer(base["VisualTracker"], "visualTrackerType", 1, 2);
    (void)tracker_type;
    require(
        !base["DataAssociator"] || !base["DataAssociator"]["dataAssociatorType"] ||
            base["DataAssociator"]["dataAssociatorType"].as<int>() == 0,
        "ReID requires the native NvDCF data associator");

    YAML::Node reid = overlay["ReID"];
    validate_keys(
        reid,
        {"reidType",
         "batchSize",
         "workspaceSize",
         "reidFeatureSize",
         "reidHistorySize",
         "inferDims",
         "networkMode",
         "inputOrder",
         "colorFormat",
         "offsets",
         "netScaleFactor",
         "keepAspc",
         "addFeatureNormalization",
         "minVisibility4GalleryUpdate",
         "modelEngineFile",
         "outputReidTensor",
         "useVPICropScaler"},
        "ReID");
    // Never inherit preprocessing from an unrelated ETLT model in the base.
    integer(reid, "batchSize", 1, 64);
    const int features = integer(reid, "reidFeatureSize", 1, 4096);
    integer(reid, "networkMode", 0, 1);
    integer(reid, "inputOrder", 0, 0);
    integer(reid, "colorFormat", 0, 1);
    integer(reid, "keepAspc", 0, 1);
    integer(reid, "addFeatureNormalization", 0, 1);
    scalar(reid, "netScaleFactor", 0.0000001, 100.0);
    require(
        reid["inferDims"].IsSequence() && reid["inferDims"].size() == 3 && reid["inferDims"][0].as<int>() == 3,
        "ReID inferDims must be RGB/BGR [3,height,width]");
    const int height = reid["inferDims"][1].as<int>();
    const int width = reid["inferDims"][2].as<int>();
    require(
        height >= 16 && height <= 1024 && width >= 16 && width <= 1024,
        "ReID image dimensions must be between 16 and 1024");
    require(reid["offsets"].IsSequence() && reid["offsets"].size() == 3, "ReID requires three offsets");
    for (const auto& offset : reid["offsets"]) {
      const double value = offset.as<double>();
      require(std::isfinite(value) && std::abs(value) <= 1000, "Invalid ReID offset");
    }
    if (!reid["workspaceSize"])
      reid["workspaceSize"] = 256;
    integer(reid, "workspaceSize", 1, 1024);
    if (!reid["reidHistorySize"])
      reid["reidHistorySize"] = 32;
    const int history = integer(reid, "reidHistorySize", 1, 128);
    const int targets = integer(base["TargetManagement"], "maxTargetsPerStream", 1, 1024);
    require(
        static_cast<uint64_t>(features) * history * targets * sizeof(float) <= 64 * 1024 * 1024,
        "ReID gallery exceeds the 64 MiB limit");
    require(
        static_cast<uint64_t>(reid["batchSize"].as<int>()) * height * width * 3 * sizeof(float) <= 64 * 1024 * 1024,
        "ReID input batch exceeds the 64 MiB limit");
    if (reid["reidType"])
      integer(reid, "reidType", 2, 2);
    if (reid["outputReidTensor"])
      integer(reid, "outputReidTensor", 0, 0);
    if (reid["useVPICropScaler"])
      integer(reid, "useVPICropScaler", 0, 1);
    if (reid["minVisibility4GalleryUpdate"])
      scalar(reid, "minVisibility4GalleryUpdate", 0, 1);
    reid["reidType"] = 2;
    reid["outputReidTensor"] = 0;
    require(reid["modelEngineFile"] && reid["modelEngineFile"].IsScalar(), "ReID requires modelEngineFile");
    fs::path engine = reid["modelEngineFile"].as<std::string>();
    require(!engine.empty(), "ReID modelEngineFile must not be empty");
    if (!engine.is_absolute())
      engine = overlay_path.parent_path() / engine;
    engine = fs::canonical(engine);
    require(fs::is_regular_file(engine) && fs::file_size(engine) > 0, "ReID requires a nonempty prebuilt engine");
    std::ifstream readable_engine(engine, std::ios::binary);
    require(readable_engine.good(), "ReID prebuilt engine is not readable");
    reid["modelEngineFile"] = engine.string();

    YAML::Node trajectory = overlay["TrajectoryManagement"];
    if (trajectory) {
      validate_keys(
          trajectory,
          {"enableReAssoc",
           "reidExtractionInterval",
           "minMatchingScore4Overall",
           "minTrackletMatchingScore",
           "minMatchingScore4ReidSimilarity",
           "matchingScoreWeight4TrackletSimilarity",
           "matchingScoreWeight4ReidSimilarity",
           "maxTrackletMatchingTimeSearchRange"},
          "TrajectoryManagement overlay");
      for (const auto& entry : trajectory) {
        const std::string key = entry.first.as<std::string>();
        if (key == "enableReAssoc")
          integer(trajectory, key.c_str(), 1, 1);
        else if (key == "reidExtractionInterval")
          integer(trajectory, key.c_str(), 1, 300);
        else if (key == "maxTrackletMatchingTimeSearchRange")
          integer(trajectory, key.c_str(), 1, 300);
        else
          scalar(trajectory, key.c_str(), 0, 1);
      }
    }
    if (!base["TrajectoryManagement"])
      base["TrajectoryManagement"] = YAML::Node(YAML::NodeType::Map);
    require(base["TrajectoryManagement"].IsMap(), "Tracker TrajectoryManagement must be a map");
    if (trajectory)
      for (const auto& entry : trajectory)
        base["TrajectoryManagement"][entry.first.as<std::string>()] = YAML::Clone(entry.second);
    base["TrajectoryManagement"]["enableReAssoc"] = 1;
    if (!trajectory || !trajectory["reidExtractionInterval"])
      base["TrajectoryManagement"]["reidExtractionInterval"] = 8;
    // Replace this model-specific section wholesale: inherited ONNX, ETLT,
    // calibration and engine-builder inputs must never reach native playback.
    base["ReID"] = YAML::Clone(reid);

    const char* temporary = std::getenv("TMPDIR");
    const fs::path root = temporary && *temporary ? temporary : "/tmp";
    std::string pattern = (root / "hstream-reid-XXXXXX").string();
    require(::mkdtemp(pattern.data()) != nullptr, "Cannot create private ReID runtime directory");
    struct EmptyDirectoryGuard {
      const char* path;
      ~EmptyDirectoryGuard() {
        ::rmdir(path);
      }
    } empty_directory_guard{pattern.c_str()};
    auto result = std::make_unique<PreparedConfig>(pattern);
    const std::string contents = "%YAML:1.0\n" + YAML::Dump(base) + "\n";
    write_config(result->path().string() + ".tmp", contents);
    fs::rename(result->path().string() + ".tmp", result->path());
    return result;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Cannot enable native tracker ReID: ") + error.what());
  } catch (...) {
    return absl::InternalError("Cannot enable native tracker ReID: unknown preparation failure");
  }
}
} // namespace hm::tracker_reid
