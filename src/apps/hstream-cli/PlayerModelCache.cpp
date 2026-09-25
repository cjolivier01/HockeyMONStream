#include "PlayerModelCache.h"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

#include "hstream/src/libs/assets/AssetManager.h"
#include "hstream/src/libs/player_analytics/Config.h"
#include "hstream/src/libs/player_analytics/ModelCatalog.h"
#include "hstream/src/libs/player_analytics/ModelContract.h"

extern char** environ;

namespace hm::pipeline {
namespace {
namespace fs = std::filesystem;
namespace pa = hm::player_analytics;
using Assets = hm::assets::AssetManager;
constexpr size_t kMaximumDocument = 1024 * 1024;
constexpr uintmax_t kMaximumEngine = 2ULL << 30;
constexpr uintmax_t kMaximumOnnx = 1ULL << 30;
constexpr const char* kCacheVersion = "player-model-cache-v1";

struct Failure : std::runtime_error {
  explicit Failure(absl::Status value) : std::runtime_error(value.ToString()), status(std::move(value)) {}
  absl::Status status;
};
void Check(absl::Status status) {
  if (!status.ok())
    throw Failure(std::move(status));
}
void Require(bool valid, const std::string& message) {
  if (!valid)
    throw Failure(absl::FailedPreconditionError(message));
}
void Cancel(const PlayerModelCacheOptions& options) {
  if (options.cancelled && options.cancelled())
    throw Failure(absl::CancelledError("Player model preparation cancelled"));
}
void Progress(const PlayerModelCacheOptions& options, std::string text) {
  Cancel(options);
  if (options.progress)
    options.progress(std::move(text));
}
bool Flag(const YAML::Node& node) {
  if (!node || node.IsNull())
    return false;
  if (node.IsScalar() && (node.Scalar() == "0" || node.Scalar() == "1"))
    return node.Scalar() == "1";
  return node.as<bool>();
}
std::string Scalar(const YAML::Node& node, std::string fallback = {}) {
  return !node || node.IsNull() ? fallback : node.as<std::string>();
}
std::string Emit(const YAML::Node& node) {
  YAML::Emitter emitter;
  emitter << node;
  Require(emitter.good(), "Cannot serialize prepared model metadata");
  return emitter.c_str();
}
std::string DigestBytes(const std::string& value) {
  auto digest = Assets::Sha256Bytes(value);
  Check(digest.status());
  return *digest;
}
std::string DigestFile(const fs::path& path, uintmax_t maximum, const PlayerModelCacheOptions& options) {
  Cancel(options);
  const auto status = fs::symlink_status(path);
  Require(fs::is_regular_file(status), "Cached model file must be regular: " + path.string());
  const auto size = fs::file_size(path);
  Require(size > 0 && size <= maximum, "Invalid cached model file size: " + path.string());
  auto digest = Assets::Sha256(path, options.cancelled);
  Check(digest.status());
  return *digest;
}
YAML::Node Document(const fs::path& path) {
  Require(fs::is_regular_file(fs::symlink_status(path)), "Model metadata is not a regular file");
  Require(fs::file_size(path) <= kMaximumDocument, "Model metadata is oversized");
  return YAML::LoadFile(path.string());
}
struct Descriptor {
  int fd{-1};
  explicit Descriptor(int value) : fd(value) {}
  ~Descriptor() {
    if (fd >= 0)
      ::close(fd);
  }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
};
void SyncFile(const fs::path& path, bool directory = false) {
  Descriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (directory ? O_DIRECTORY : 0)));
  Require(file.fd >= 0 && ::fsync(file.fd) == 0, "Cannot durably flush prepared model: " + path.string());
}
void Write(const fs::path& path, const std::string& text) {
  Descriptor file(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
  Require(file.fd >= 0, "Cannot create model metadata: " + path.string());
  size_t done = 0;
  while (done < text.size()) {
    const auto count = ::write(file.fd, text.data() + done, text.size() - done);
    if (count < 0 && errno == EINTR)
      continue;
    Require(count > 0, "Cannot write model metadata");
    done += static_cast<size_t>(count);
  }
  Require(::fsync(file.fd) == 0, "Cannot durably flush model metadata");
}
class Lock {
 public:
  Lock(const fs::path& path, const PlayerModelCacheOptions& options)
      : descriptor_(::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600)) {
    Require(descriptor_.fd >= 0, "Cannot open player model cache lock");
    while (::flock(descriptor_.fd, LOCK_EX | LOCK_NB) != 0) {
      Require(errno == EINTR || errno == EWOULDBLOCK, "Cannot lock player model cache");
      Cancel(options);
      ::poll(nullptr, 0, 50);
    }
    Cancel(options);
  }

 private:
  Descriptor descriptor_;
};
struct TemporaryDirectory {
  fs::path path;
  explicit TemporaryDirectory(const fs::path& parent, const std::string& prefix = ".preparing-") {
    auto pattern = (parent / (prefix + "XXXXXX")).string();
    Require(::mkdtemp(pattern.data()) != nullptr, "Cannot create private model preparation directory");
    path = pattern;
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};

// Every helper has its own process group, so cancellation also terminates any
// descendants. The native helper additionally sets PDEATHSIG for abrupt exit.
class Child {
 public:
  pid_t pid{-1};
  bool reaped{false};
  ~Child() {
    Stop();
  }
  void Stop() noexcept {
    if (pid <= 0)
      return;
    if (reaped) {
      ::kill(-pid, SIGKILL);
      return;
    }
    ::kill(-pid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    int status;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto result = ::waitpid(pid, &status, WNOHANG);
      if (result == pid || (result < 0 && errno == ECHILD)) {
        reaped = true;
        // Descendants may outlive a promptly exiting parent.
        ::kill(-pid, SIGKILL);
        return;
      }
      ::poll(nullptr, 0, 10);
    }
    ::kill(-pid, SIGKILL);
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    reaped = true;
  }
};

std::string Run(
    const fs::path& executable,
    const std::vector<std::string>& arguments,
    const PlayerModelCacheOptions& options,
    std::chrono::seconds timeout) {
  Cancel(options);
  int output_pipe[2];
  Require(::pipe2(output_pipe, O_CLOEXEC) == 0, "Cannot open model helper pipe");
  Descriptor read_end(output_pipe[0]), write_end(output_pipe[1]);
  Require(::fcntl(read_end.fd, F_SETFL, O_NONBLOCK) == 0, "Cannot configure model helper pipe");
  posix_spawn_file_actions_t actions;
  Require(::posix_spawn_file_actions_init(&actions) == 0, "Cannot configure model helper");
  struct ActionCleanup {
    posix_spawn_file_actions_t* value;
    ~ActionCleanup() {
      ::posix_spawn_file_actions_destroy(value);
    }
  } actions_cleanup{&actions};
  Require(
      ::posix_spawn_file_actions_adddup2(&actions, write_end.fd, STDOUT_FILENO) == 0 &&
          ::posix_spawn_file_actions_adddup2(&actions, write_end.fd, STDERR_FILENO) == 0 &&
          ::posix_spawn_file_actions_addclose(&actions, read_end.fd) == 0 &&
          ::posix_spawn_file_actions_addclose(&actions, write_end.fd) == 0,
      "Cannot redirect model helper output");
  posix_spawnattr_t attributes;
  Require(::posix_spawnattr_init(&attributes) == 0, "Cannot configure model helper group");
  struct AttributeCleanup {
    posix_spawnattr_t* value;
    ~AttributeCleanup() {
      ::posix_spawnattr_destroy(value);
    }
  } attributes_cleanup{&attributes};
  sigset_t empty, defaults;
  ::sigemptyset(&empty);
  ::sigemptyset(&defaults);
  ::sigaddset(&defaults, SIGINT);
  ::sigaddset(&defaults, SIGTERM);
  Require(
      ::posix_spawnattr_setpgroup(&attributes, 0) == 0 && ::posix_spawnattr_setsigmask(&attributes, &empty) == 0 &&
          ::posix_spawnattr_setsigdefault(&attributes, &defaults) == 0 &&
          ::posix_spawnattr_setflags(
              &attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF) == 0,
      "Cannot configure model helper signals");
  std::vector<std::string> strings{executable.string(), "--parent-pid", std::to_string(::getpid())};
  strings.insert(strings.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  for (auto& value : strings)
    argv.push_back(value.data());
  argv.push_back(nullptr);
  Child child;
  const int error = ::posix_spawn(&child.pid, executable.c_str(), &actions, &attributes, argv.data(), environ);
  Require(error == 0, "Cannot start player-model-builder: " + std::string(std::strerror(error)));
  ::close(write_end.fd);
  write_end.fd = -1;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string output;
  bool eof = false;
  int status = 0;
  while (!child.reaped || !eof) {
    Cancel(options);
    Require(std::chrono::steady_clock::now() < deadline, "Player model helper timed out");
    std::array<char, 8192> buffer;
    for (int reads = 0; reads < 32; ++reads) {
      const auto count = ::read(read_end.fd, buffer.data(), buffer.size());
      if (count > 0) {
        output.append(buffer.data(), count);
        // Keep diagnostics bounded while retaining final JSON and error text.
        if (output.size() > kMaximumDocument)
          output.erase(0, output.size() - kMaximumDocument);
      } else {
        eof = count == 0;
        Require(count >= 0 || errno == EAGAIN || errno == EINTR, "Cannot read model helper output");
        break;
      }
    }
    if (!child.reaped) {
      const auto result = ::waitpid(child.pid, &status, WNOHANG);
      Require(result >= 0 || errno == EINTR, "Cannot wait for model helper");
      child.reaped = result == child.pid;
    }
    if (!child.reaped || !eof)
      ::poll(nullptr, 0, 20);
  }
  Require(
      WIFEXITED(status) && WEXITSTATUS(status) == 0,
      "Player model preparation failed: " + output.substr(output.size() > 8192 ? output.size() - 8192 : 0));
  return output;
}
YAML::Node LastReport(const std::string& output) {
  size_t end = output.size();
  while (end > 0) {
    const size_t begin = output.rfind('\n', end - 1);
    const auto line =
        output.substr(begin == std::string::npos ? 0 : begin + 1, end - (begin == std::string::npos ? 0 : begin + 1));
    if (!line.empty() && line.front() == '{') {
      const auto result = YAML::Load(line);
      Require(result.IsMap(), "Invalid model helper report");
      return result;
    }
    if (begin == std::string::npos)
      break;
    end = begin;
  }
  throw Failure(absl::DataLossError("Player model helper did not report validation results"));
}
YAML::Node Identity(const fs::path& builder, int gpu, const PlayerModelCacheOptions& options) {
  const auto report =
      LastReport(Run(builder, {"--runtime-info", "--gpu-id", std::to_string(gpu)}, options, std::chrono::seconds(60)));
  YAML::Node result;
  for (const char* field : {"tensorrt_version", "gpu_name", "compute_capability"}) {
    const auto value = report[field].as<std::string>();
    Require(
        !value.empty() && value.size() <= 256 && value.find('\0') == std::string::npos,
        "Invalid model helper runtime identity");
    result[field] = value;
  }
  for (const char* field : {"tensorrt_build_version", "cuda_runtime_version"}) {
    const auto value = report[field].as<int>();
    Require(
        value >= 0 && (std::string(field) != "cuda_runtime_version" || value > 0),
        "Invalid model helper runtime version");
    result[field] = value;
  }
  return result;
}
bool SameIdentity(const YAML::Node& left, const YAML::Node& right) {
  for (const char* field :
       {"tensorrt_version", "gpu_name", "compute_capability", "tensorrt_build_version", "cuda_runtime_version"})
    if (Scalar(left[field]) != Scalar(right[field]))
      return false;
  return true;
}
fs::path Builder(const PlayerModelCacheOptions& options) {
  if (!options.builder_path.empty()) {
    Require(::access(options.builder_path.c_str(), X_OK) == 0, "Player model builder override is not executable");
    return fs::absolute(options.builder_path);
  }
  const auto executable = fs::read_symlink("/proc/self/exe");
  const auto parent = executable.parent_path();
  std::vector<fs::path> candidates = {
      parent / "player-model-builder",
      parent.parent_path() / "player-model-builder/player-model-builder",
      fs::path(executable.string() + ".runfiles/kstream/src/apps/player-model-builder/player-model-builder")};
  for (const auto& candidate : candidates)
    if (::access(candidate.c_str(), X_OK) == 0)
      return fs::absolute(candidate);
  throw Failure(absl::NotFoundError("Native player-model-builder is unavailable beside hstream-cli"));
}
fs::path CacheRoot(const PlayerModelCacheOptions& options) {
  if (!options.cache_root.empty())
    return fs::weakly_canonical(fs::absolute(options.cache_root));
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
    Require(fs::path(xdg).is_absolute(), "XDG_CACHE_HOME must be absolute");
    return fs::weakly_canonical(fs::path(xdg) / "hstream/player-models");
  }
  const char* home = std::getenv("HOME");
  Require(home && *home, "Cannot resolve the user model cache directory");
  return fs::weakly_canonical(fs::path(home) / ".cache/hstream/player-models");
}
YAML::Node Manifest(const pa::CatalogModel& model, const YAML::Node& identity, const std::string& engine_digest) {
  auto manifest = YAML::Load(model.contract_json);
  manifest["engine"] = YAML::Clone(identity);
  manifest["engine"]["file"] = "model.engine";
  manifest["engine"]["sha256"] = engine_digest;
  manifest["engine"]["precision"] = "fp16";
  manifest["engine"]["max_batch"] = model.max_batch;
  if (std::string(model.feature) != "reid") {
    auto parsed = pa::ParseModelManifest(manifest);
    Check(parsed.status());
    Require(
        parsed->onnx_file == "model.onnx" && parsed->onnx_sha256 == model.onnx_sha256 && parsed->model_id == model.id,
        "Built-in model contract does not match the catalog");
  }
  return manifest;
}
YAML::Node TrackerOverlay(const pa::CatalogModel& model, const fs::path& bundle) {
  const auto contract = YAML::Load(model.contract_json);
  auto overlay = YAML::Clone(contract["tracker"]);
  Require(overlay.IsMap() && overlay["ReID"].IsMap(), "Built-in ReID tracker contract is missing");
  overlay["ReID"]["modelEngineFile"] = (bundle / "model.engine").string();
  return overlay;
}

bool ValidBundle(
    const fs::path& bundle,
    const pa::CatalogModel& model,
    const YAML::Node& identity,
    const PlayerModelCacheOptions& options) {
  try {
    if (!fs::is_directory(fs::symlink_status(bundle)))
      return false;
    const auto record = Document(bundle / "preparation.yaml");
    if (Scalar(record["schema"]) != kCacheVersion || Scalar(record["model"]) != model.id ||
        Scalar(record["contract_sha256"]) != DigestBytes(model.contract_json) || !Flag(record["validated"]) ||
        !SameIdentity(record["runtime"], identity) || record["max_batch"].as<int>() != model.max_batch)
      return false;
    const char* source_file = std::string(model.id) == "deepstream" ? "model.etlt" : "model.onnx";
    if (DigestFile(bundle / source_file, kMaximumOnnx, options) != model.onnx_sha256)
      return false;
    const auto engine_digest = DigestFile(bundle / "model.engine", kMaximumEngine, options);
    if (engine_digest != Scalar(record["engine_sha256"]))
      return false;
    const auto manifest = Manifest(model, identity, engine_digest);
    if (Emit(Document(bundle / "manifest.json")) != Emit(manifest))
      return false;
    if (std::string(model.feature) == "reid" &&
        Emit(Document(bundle / "tracker.yaml")) != Emit(TrackerOverlay(model, bundle)))
      return false;
    return true;
  } catch (const Failure& failure) {
    if (absl::IsCancelled(failure.status))
      throw;
    return false;
  } catch (const std::exception&) {
    return false;
  }
}

fs::path Prepare(
    const pa::CatalogModel& model,
    int gpu,
    const YAML::Node& identity,
    const fs::path& builder,
    const fs::path& root,
    const PlayerModelCacheOptions& options,
    const fs::path& installed_source = {}) {
  const std::string key = DigestBytes(
      std::string(kCacheVersion) + "\n" + model.id + "\n" + model.onnx_sha256 + "\n" + model.contract_json + "\n" +
      Emit(identity) + "\nfp16\n" + std::to_string(model.max_batch));
  fs::create_directories(root / "bundles");
  fs::create_directories(root / "locks");
  const fs::path bundle = root / "bundles" / key;
  Lock lock(root / "locks" / (key + ".lock"), options);
  if (ValidBundle(bundle, model, identity, options)) {
    Progress(options, "Using prepared " + std::string(model.label));
    return bundle;
  }
  Progress(options, "Downloading " + std::string(model.label));
  const bool deepstream = std::string(model.id) == "deepstream";
  const char* source_file = deepstream ? "model.etlt" : "model.onnx";
  fs::path onnx = root / "models" / model.onnx_sha256 / source_file;
  if (!installed_source.empty() && fs::is_regular_file(installed_source) &&
      DigestFile(fs::canonical(installed_source), kMaximumOnnx, options) == model.onnx_sha256)
    onnx = fs::canonical(installed_source);
  hm::assets::AssetSpec asset;
  asset.name = model.id;
  asset.url = model.onnx_url;
  asset.sha256 = model.onnx_sha256;
  asset.target = onnx;
  Check(Assets::EnsureAsset(asset, {}, options.cancelled));
  Cancel(options);
  TemporaryDirectory staging(root / "bundles");
  fs::copy_file(onnx, staging.path / source_file);
  Require(
      DigestFile(staging.path / source_file, kMaximumOnnx, options) == model.onnx_sha256,
      "Downloaded model changed while preparing its bundle");
  Write(staging.path / "contract.json", model.contract_json);
  Progress(options, "Preparing " + std::string(model.label) + " for this GPU; the first run can take several minutes");
  if (deepstream) {
    const auto contract = YAML::Load(model.contract_json);
    auto native = YAML::Clone(contract["tracker_base"]);
    const auto overlay = TrackerOverlay(model, staging.path);
    native["ReID"] = YAML::Clone(overlay["ReID"]);
    native["ReID"]["tltEncodedModel"] = (staging.path / source_file).string();
    native["ReID"]["tltModelKey"] = "nvidia_tao";
    native["TrajectoryManagement"]["enableReAssoc"] = 1;
    native["TrajectoryManagement"]["reidExtractionInterval"] = 8;
    Write(staging.path / "tracker-build.yaml", Emit(native));
    const auto report = LastReport(
        Run(builder,
            {"--gpu-id",
             std::to_string(gpu),
             "--tracker-config",
             (staging.path / "tracker-build.yaml").string(),
             "--tracker-library",
             contract["tracker_library"].as<std::string>()},
            options,
            std::chrono::minutes(30)));
    Require(
        Flag(report["tracker_prepared"]) && SameIdentity(report["runtime"], identity),
        "Native tracker helper did not verify the selected runtime identity");
    // DeepStream's TAO builder derives this filename from its staged ETLT input
    // even when modelEngineFile names a different initially missing file.
    const auto derived = staging.path /
        (std::string(source_file) + "_b" + std::to_string(model.max_batch) + "_gpu" + std::to_string(gpu) +
         "_fp16.engine");
    if (!fs::is_regular_file(staging.path / "model.engine") && fs::is_regular_file(derived))
      fs::rename(derived, staging.path / "model.engine");
    fs::remove(staging.path / "tracker-build.yaml");
  } else {
    const auto report = LastReport(
        Run(builder,
            {"--gpu-id",
             std::to_string(gpu),
             "--onnx",
             (staging.path / source_file).string(),
             "--engine",
             (staging.path / "model.engine").string(),
             "--precision",
             "fp16",
             "--max-batch",
             std::to_string(model.max_batch),
             "--contract",
             (staging.path / "contract.json").string()},
            options,
            std::chrono::minutes(30)));
    Require(
        Flag(report["validated"]) && report["batch"].as<int>() == model.max_batch &&
            SameIdentity(report["runtime"], identity),
        "Model builder did not validate the selected GPU, contract and maximum batch");
  }
  const auto engine_digest = DigestFile(staging.path / "model.engine", kMaximumEngine, options);
  Write(staging.path / "manifest.json", Emit(Manifest(model, identity, engine_digest)));
  if (std::string(model.feature) == "reid")
    Write(staging.path / "tracker.yaml", Emit(TrackerOverlay(model, bundle)));
  YAML::Node record;
  record["schema"] = kCacheVersion;
  record["model"] = model.id;
  record["contract_sha256"] = DigestBytes(model.contract_json);
  record["engine_sha256"] = engine_digest;
  record["runtime"] = YAML::Clone(identity);
  record["max_batch"] = model.max_batch;
  record["validated"] = true;
  Write(staging.path / "preparation.yaml", Emit(record));
  SyncFile(staging.path / source_file);
  SyncFile(staging.path / "model.engine");
  SyncFile(staging.path, true);
  Cancel(options);
  // Invalid generations cannot be reused. Keep publication atomic for all
  // callers of this cache, which hold the same per-identity lock.
  if (fs::exists(fs::symlink_status(bundle))) {
    TemporaryDirectory obsolete(root / "bundles", ".invalid-");
    fs::rename(bundle, obsolete.path / "bundle");
    fs::rename(staging.path, bundle);
  } else {
    fs::rename(staging.path, bundle);
  }
  SyncFile(root / "bundles", true);
  Require(ValidBundle(bundle, model, identity, options), "Published model cache failed verification");
  Progress(options, "Prepared " + std::string(model.label));
  return bundle;
}

#include "DeepStreamReIdCache.inc"

int Gpu(const YAML::Node& node, int fallback) {
  const int value = node && !node.IsNull() ? node.as<int>() : fallback;
  Require(value >= 0, "Player model GPU id must be nonnegative");
  return value;
}
} // namespace

absl::Status PreparePlayerModelCache(
    YAML::Node pipeline,
    const fs::path& config_directory,
    const PlayerModelCacheOptions& options) {
  try {
    const YAML::Node resolved = pipeline;
    const auto analytics = resolved["player-analytics"];
    const auto tracker = resolved["tracker"];
    bool enabled = false;
    if (analytics && analytics.IsMap())
      for (const char* feature : {"pose", "jersey", "action"})
        enabled = enabled || (analytics[feature] && Flag(analytics[feature]["enable"]));
    const bool reid = tracker && Flag(tracker["enable"]) && Flag(tracker["reid-enable"]);
    // Deliberately precedes all paths, environment access, callbacks and GPU work.
    if (!enabled && !reid)
      return absl::OkStatus();
    if (!tracker || !Flag(tracker["enable"]))
      return absl::FailedPreconditionError("Player analytics and ReID require pipeline.tracker.enable=1");
    auto parsed = pa::ParseConfig(analytics);
    Check(parsed.status());
    struct Selection {
      const pa::CatalogModel* model;
      int gpu;
    };
    std::vector<Selection> selected;
    const auto application = resolved["application"];
    const auto global = application ? application["global-gpu-id"] : YAML::Node();
    const int default_gpu = global && !global.IsNull() ? std::max(0, global.as<int>()) : 0;
    for (const auto& entry : std::vector<std::pair<const char*, const pa::FeatureConfig*>>{
             {"pose", &parsed->pose}, {"jersey", &parsed->jersey}, {"action", &parsed->action}}) {
      if (!entry.second->enabled || entry.second->model == "custom")
        continue;
      const auto* model = options.model_lookup ? options.model_lookup(entry.first, entry.second->model)
                                               : pa::FindModel(entry.first, entry.second->model);
      Require(model, "Unsupported built-in player model");
      selected.push_back({model, Gpu(analytics["gpu-id"], default_gpu)});
    }
    if (reid) {
      auto id = Scalar(tracker["reid-model"]);
      if (!tracker["reid-model"] || tracker["reid-model"].IsNull())
        id = !Scalar(tracker["reid-config-file"]).empty() ? "custom" : std::string(pa::DefaultModelId("reid"));
      if (id == "custom") {
        Require(!Scalar(tracker["reid-config-file"]).empty(), "Custom ReID requires reid-config-file");
      } else {
        const auto* model = options.model_lookup ? options.model_lookup("reid", id) : pa::FindModel("reid", id);
        Require(model, "Unsupported built-in ReID model");
        Require(
            fs::path(Scalar(tracker["ll-lib-file"])).filename() == "libnvds_nvmultiobjecttracker.so",
            "ReID requires the NVIDIA native multiobject tracker library");
        Require(Scalar(tracker["sub-batches"]).empty(), "ReID does not support tracker sub-batches");
        selected.push_back({model, Gpu(tracker["gpu-id"], default_gpu)});
      }
    }
    if (selected.empty())
      return absl::OkStatus();
    Cancel(options);
    const auto builder = Builder(options);
    const auto root = CacheRoot(options);
    std::map<int, YAML::Node> identities;
    struct Prepared {
      std::string feature, model;
      fs::path directory;
    };
    std::vector<Prepared> prepared;
    for (const auto& selection : selected) {
      if (!identities.count(selection.gpu))
        identities[selection.gpu] = Identity(builder, selection.gpu, options);
      const auto directory = std::string(selection.model->id) == "deepstream"
          ? PrepareDeepStream(
                tracker, config_directory, selection.gpu, identities.at(selection.gpu), builder, root, options)
          : Prepare(*selection.model, selection.gpu, identities.at(selection.gpu), builder, root, options);
      prepared.push_back({selection.model->feature, selection.model->id, directory});
    }
    Cancel(options);
    for (const auto& entry : prepared) {
      if (entry.feature == "reid") {
        pipeline["tracker"]["reid-config-file"] = (entry.directory / "tracker.yaml").string();
        pipeline["tracker"]["reid-model"] = entry.model;
      } else {
        pipeline["player-analytics"][entry.feature]["bundle"] = entry.directory.string();
        pipeline["player-analytics"][entry.feature]["model"] = entry.model;
      }
    }
    return absl::OkStatus();
  } catch (const Failure& failure) {
    return failure.status;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Player model preparation: ") + error.what());
  }
}
} // namespace hm::pipeline
