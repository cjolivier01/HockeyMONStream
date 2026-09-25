#include "PlayerModelCache.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "hstream/src/libs/assets/AssetManager.h"
#include "hstream/src/libs/player_analytics/ModelCatalog.h"

namespace {
namespace fs = std::filesystem;
namespace pa = hm::player_analytics;
using hm::pipeline::PlayerModelCacheOptions;
using hm::pipeline::PreparePlayerModelCache;
void Expect(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Write(const fs::path& path, const std::string& value) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << value;
  Expect(output.good(), "Cannot write test fixture");
}
std::string Read(const fs::path& path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void Journal(const std::string& text) {
  const char* path = std::getenv("HM_TEST_PLAYER_MODEL_HELPER_LOG");
  if (!path)
    return;
  const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
  const auto line = text + "\n";
  Expect(
      fd >= 0 && ::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size()), "Cannot journal helper");
  ::close(fd);
}
std::string Runtime(int gpu) {
  return "{\"tensorrt_version\":\"10.3.0\",\"tensorrt_build_version\":30,\"cuda_runtime_version\":12060,"
         "\"gpu_name\":\"Test GPU " +
      std::to_string(gpu) + "\",\"compute_capability\":\"8.7\"}";
}
int Helper(int argc, char** argv) {
  std::map<std::string, std::string> values;
  bool query = false;
  for (int i = 1; i < argc; ++i) {
    const std::string name = argv[i];
    if (name == "--runtime-info")
      query = true;
    else {
      Expect(i + 1 < argc, "Missing fake helper value");
      values[name] = argv[++i];
    }
  }
  Expect(values.count("--gpu-id"), "Selected GPU was not sent to helper");
  const int gpu = std::stoi(values.at("--gpu-id"));
  if (query) {
    Journal("query " + std::to_string(gpu));
    std::cout << Runtime(gpu) << '\n';
    return 0;
  }
  Expect(values.at("--precision") == "fp16", "Automatic model precision must be FP16");
  Expect(fs::is_regular_file(values.at("--contract")), "No binding contract supplied to builder");
  const auto contract = YAML::LoadFile(values.at("--contract"));
  Expect(contract["inputs"].size() == 1 && contract["outputs"].size() > 0, "Missing IO contract");
  Journal("build " + std::to_string(gpu));
  if (std::getenv("HM_TEST_PLAYER_MODEL_HELPER_SLEEP")) {
    const auto descendant = ::fork();
    Expect(descendant >= 0, "Cannot fork cancellation descendant");
    if (descendant == 0) {
      for (;;)
        ::pause();
    }
    Journal("sleeping " + std::to_string(::getpid()) + " " + std::to_string(descendant));
    for (;;)
      ::pause();
  }
  if (std::getenv("HM_TEST_PLAYER_MODEL_HELPER_FAIL"))
    return 42;
  Write(values.at("--engine"), "test engine for gpu " + std::to_string(gpu));
  std::cout << "{\"runtime\":" << Runtime(gpu) << ",\"validated\":true,\"batch\":" << values.at("--max-batch")
            << ",\"outputs\":[]}\n";
  return 0;
}
size_t Builds(const fs::path& journal) {
  std::istringstream stream(Read(journal));
  std::string line;
  size_t count = 0;
  while (std::getline(stream, line))
    count += line.rfind("build ", 0) == 0;
  return count;
}
struct Fixture {
  fs::path directory, journal;
  std::string digest, contract;
  pa::CatalogModel model{};
  PlayerModelCacheOptions options;
  Fixture() {
    std::string pattern = "/tmp/hstream-player-cache-test-XXXXXX";
    Expect(::mkdtemp(pattern.data()) != nullptr, "Cannot create test root");
    directory = pattern;
    journal = directory / "helper.log";
    ::setenv("HM_TEST_PLAYER_MODEL_HELPER_LOG", journal.c_str(), 1);
    const std::string bytes = "small CPU-only model preparation fixture";
    digest = *hm::assets::AssetManager::Sha256Bytes(bytes);
    model = *pa::FindModel("pose", pa::DefaultModelId("pose"));
    auto node = YAML::Load(model.contract_json);
    node["onnx"]["sha256"] = digest;
    YAML::Emitter emitter;
    emitter << node;
    contract = emitter.c_str();
    model.onnx_url = "https://invalid.invalid/test-only-model.onnx";
    model.onnx_sha256 = digest.c_str();
    model.contract_json = contract.c_str();
    options.builder_path = fs::read_symlink("/proc/self/exe");
    options.cache_root = directory / "cache";
    options.model_lookup = [this](std::string_view feature, std::string_view id) {
      return feature == "pose" && id == model.id ? &model : pa::FindModel(feature, id);
    };
    Write(options.cache_root / "models" / digest / "model.onnx", bytes);
  }
  ~Fixture() {
    ::unsetenv("HM_TEST_PLAYER_MODEL_HELPER_LOG");
    ::unsetenv("HM_TEST_PLAYER_MODEL_HELPER_SLEEP");
    ::unsetenv("HM_TEST_PLAYER_MODEL_HELPER_FAIL");
    std::error_code ignored;
    fs::remove_all(directory, ignored);
  }
  YAML::Node Pipeline(int gpu = 0) const {
    auto node = YAML::Load("tracker: {enable: 1}\nplayer-analytics: {pose: {enable: 1}}\n");
    node["player-analytics"]["gpu-id"] = gpu;
    return node;
  }
  void NoStaging() const {
    for (const auto& entry : fs::directory_iterator(options.cache_root / "bundles"))
      Expect(
          entry.path().filename().string().rfind(".preparing-", 0) != 0,
          "Temporary model leaked after cancellation/failure");
  }
};
void OffAndCustom() {
  PlayerModelCacheOptions forbidden;
  forbidden.builder_path = "/must-not-open/helper";
  forbidden.cache_root = "/must-not-create/cache";
  forbidden.cancelled = [] {
    throw std::runtime_error("Disabled path consulted cancellation callback");
    return false;
  };
  forbidden.progress = [](std::string) { throw std::runtime_error("Disabled path reported model work"); };
  forbidden.model_lookup = [](std::string_view, std::string_view) -> const pa::CatalogModel* {
    throw std::runtime_error("Disabled path consulted catalog");
  };
  for (
      const char* source :
      {"{}",
       "tracker: {enable: 0, reid-enable: 1, reid-model: [bad], reid-config-file: [bad]}",
       "tracker: {reid-enable: 0, reid-model: broken, reid-config-file: [bad]}\nplayer-analytics: {pose: {enable: 0, model: broken, bundle: [bad]}, gpu-id: bad}",
       "player-analytics: {draw-pose: 1, draw-jerseys: 1, draw-actions: 1}",
       "tracker: {enable: 1}\nplayer-analytics: {pose: {enable: 1, bundle: /missing/custom}}",
       "tracker: {enable: 1, reid-enable: 1, reid-model: custom, reid-config-file: /missing/custom}"}) {
    auto node = YAML::Load(source);
    const auto status = PreparePlayerModelCache(node, "/unused/config", forbidden);
    Expect(status.ok(), status.ToString());
  }
  auto no_tracker = YAML::Load("player-analytics: {pose: {enable: 1}}");
  Expect(
      absl::IsFailedPrecondition(PreparePlayerModelCache(no_tracker, "/unused", forbidden)),
      "Tracker prerequisite did not fail before callbacks/helper");
  auto action_only = YAML::Load("tracker: {enable: 1}\nplayer-analytics: {action: {enable: 1}}");
  Expect(
      !PreparePlayerModelCache(action_only, "/unused", forbidden).ok(),
      "Invalid action dependency reached preparation");
}
void ReuseAndCorruption() {
  Fixture fixture;
  auto pipeline = fixture.Pipeline(2);
  auto status = PreparePlayerModelCache(pipeline, fixture.directory, fixture.options);
  Expect(status.ok(), status.ToString());
  const auto bundle = fs::path(pipeline["player-analytics"]["pose"]["bundle"].as<std::string>());
  Expect(
      fs::is_regular_file(bundle / "manifest.json") && fs::is_regular_file(bundle / "preparation.yaml"),
      "Preparation did not publish complete bundle");
  Expect(Read(fixture.journal).find("query 2") != std::string::npos, "GPU identity queried on wrong device");
  Expect(Builds(fixture.journal) == 1, "Expected one initial engine preparation");
  status = PreparePlayerModelCache(pipeline, fixture.directory, fixture.options);
  Expect(status.ok(), status.ToString());
  Expect(Builds(fixture.journal) == 1, "Verified cache did not reuse engine");
  Write(bundle / "model.engine", "corrupted engine");
  status = PreparePlayerModelCache(pipeline, fixture.directory, fixture.options);
  Expect(status.ok(), status.ToString());
  Expect(Builds(fixture.journal) == 2, "Corrupted engine cache was reused");
  auto other_gpu = fixture.Pipeline(3);
  status = PreparePlayerModelCache(other_gpu, fixture.directory, fixture.options);
  Expect(status.ok(), status.ToString());
  Expect(
      other_gpu["player-analytics"]["pose"]["bundle"].as<std::string>() != bundle.string(),
      "Different GPU runtime identities shared a bundle");
  auto stale = fixture.Pipeline(2);
  stale["player-analytics"]["pose"]["model"] = fixture.model.id;
  stale["player-analytics"]["pose"]["bundle"] = YAML::Load("[malformed, legacy]");
  status = PreparePlayerModelCache(stale, fixture.directory, fixture.options);
  Expect(status.ok(), status.ToString());
  Expect(
      stale["player-analytics"]["pose"]["bundle"].as<std::string>() == bundle.string(),
      "Explicit built-in did not replace malformed legacy path");
  fixture.NoStaging();
}
void FailedPreparation() {
  Fixture fixture;
  auto pipeline = fixture.Pipeline();
  ::setenv("HM_TEST_PLAYER_MODEL_HELPER_FAIL", "1", 1);
  const auto status = PreparePlayerModelCache(pipeline, fixture.directory, fixture.options);
  Expect(!status.ok(), "Failed builder was accepted");
  Expect(!pipeline["player-analytics"]["pose"]["bundle"], "Failed build changed the runtime config");
  Expect(fs::is_empty(fixture.options.cache_root / "bundles"), "Failed build published artifacts");
  fixture.NoStaging();
}
void CancelledPreparation() {
  Fixture fixture;
  auto pipeline = fixture.Pipeline();
  ::setenv("HM_TEST_PLAYER_MODEL_HELPER_SLEEP", "1", 1);
  fixture.options.cancelled = [&] { return Read(fixture.journal).find("sleeping ") != std::string::npos; };
  const auto started = std::chrono::steady_clock::now();
  const auto status = PreparePlayerModelCache(pipeline, fixture.directory, fixture.options);
  Expect(absl::IsCancelled(status), "Cancelled helper did not return cancellation");
  Expect(std::chrono::steady_clock::now() - started < std::chrono::seconds(5), "Cancellation was not bounded");
  Expect(!pipeline["player-analytics"]["pose"]["bundle"], "Cancellation changed runtime model path");
  const auto log = Read(fixture.journal);
  std::istringstream pids(log.substr(log.find("sleeping ") + 9));
  pid_t helper = 0, descendant = 0;
  pids >> helper >> descendant;
  Expect(helper > 0 && descendant > 0, "Cancellation fixture did not start helper group");
  Expect(::kill(helper, 0) < 0, "Cancelled helper was not reaped");
  // A killed orphan may briefly remain a zombie until PID 1 reaps it.
  const auto descendant_status = Read(fs::path("/proc") / std::to_string(descendant) / "status");
  Expect(
      descendant_status.empty() || descendant_status.find("State:\tZ") != std::string::npos,
      "Cancelled helper descendant is still running");
  fixture.NoStaging();
}
void CancelledAssetLock() {
  Fixture fixture;
  hm::assets::AssetSpec spec;
  spec.name = "test";
  spec.url = "https://invalid.invalid/never-download";
  spec.sha256 = fixture.digest;
  spec.target = fixture.directory / "missing.onnx";
  const int lock = ::open((spec.target.string() + ".lock").c_str(), O_CREAT | O_RDWR, 0600);
  Expect(lock >= 0 && ::flock(lock, LOCK_EX | LOCK_NB) == 0, "Cannot hold asset test lock");
  const auto start = std::chrono::steady_clock::now();
  const auto result = hm::assets::AssetManager::EnsureAsset(
      spec, {}, [&] { return std::chrono::steady_clock::now() - start > std::chrono::milliseconds(50); });
  ::close(lock);
  Expect(absl::IsCancelled(result), "Asset lock wait ignored cancellation");
  Expect(!fs::exists(spec.target), "Cancelled asset published a file");
}
void CancelledCacheLock() {
  Fixture fixture;
  auto pipeline = fixture.Pipeline();
  auto status = PreparePlayerModelCache(pipeline, fixture.directory, fixture.options);
  Expect(status.ok(), status.ToString());
  const fs::path bundle = pipeline["player-analytics"]["pose"]["bundle"].as<std::string>();
  const fs::path lock_path = fixture.options.cache_root / "locks" / (bundle.filename().string() + ".lock");
  const int lock = ::open(lock_path.c_str(), O_RDWR);
  Expect(lock >= 0 && ::flock(lock, LOCK_EX | LOCK_NB) == 0, "Cannot hold model test lock");
  const auto start = std::chrono::steady_clock::now();
  fixture.options.cancelled = [&] { return std::chrono::steady_clock::now() - start > std::chrono::milliseconds(100); };
  status = PreparePlayerModelCache(pipeline, fixture.directory, fixture.options);
  ::close(lock);
  Expect(absl::IsCancelled(status), "Cache lock wait ignored cancellation");
  Expect(Builds(fixture.journal) == 1, "Cancelled cache waiter ran a new build");
}
void ConcurrentPreparation() {
  Fixture fixture;
  const pid_t child = ::fork();
  Expect(child >= 0, "Cannot fork cache concurrency test");
  if (child == 0) {
    auto pipeline = fixture.Pipeline();
    const auto status = PreparePlayerModelCache(pipeline, fixture.directory, fixture.options);
    ::_exit(status.ok() ? 0 : 1);
  }
  auto pipeline = fixture.Pipeline();
  const auto status = PreparePlayerModelCache(pipeline, fixture.directory, fixture.options);
  int child_status = -1;
  Expect(::waitpid(child, &child_status, 0) == child, "Cannot wait for parallel cache caller");
  Expect(status.ok() && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0, "Concurrent cache caller failed");
  Expect(Builds(fixture.journal) == 1, "Concurrent callers built the same engine twice");
  fixture.NoStaging();
}
} // namespace
int main(int argc, char** argv) {
  try {
    if (argc > 1)
      return Helper(argc, argv);
    OffAndCustom();
    ReuseAndCorruption();
    FailedPreparation();
    CancelledPreparation();
    ConcurrentPreparation();
    CancelledAssetLock();
    CancelledCacheLock();
    std::cout << "Player model cache checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
