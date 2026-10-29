#include "hstream/src/libs/publication/Publication.h"
#include "hstream/src/libs/publication/Cleanup.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <regex>
#include <set>
#include <stdexcept>
#include <utility>

#include "hstream/src/libs/common/PinnedFile.h"
#include "hstream/src/libs/recording/Database.h"

namespace hm::publication {
namespace {
struct Fd {
  int value{-1};
  explicit Fd(int fd = -1) : value(fd) {}
  ~Fd() {
    if (value >= 0)
      ::close(value);
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  void close() {
    if (value >= 0)
      ::close(std::exchange(value, -1));
  }
};
[[noreturn]] void fail(const std::string& message) {
  throw std::runtime_error(message);
}
void check(bool success, const std::string& message) {
  if (!success)
    fail(message + ": " + std::strerror(errno));
}
void sync_directory(const fs::path& directory) {
  Fd fd(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  check(fd.value >= 0 && ::fsync(fd.value) == 0, "Cannot synchronize " + directory.string());
}
struct stat info(const fs::path& path) {
  struct stat st{};
  check(::lstat(path.c_str(), &st) == 0, "Cannot inspect " + path.string());
  return st;
}
bool same_mount(const fs::path& left, const fs::path& right) {
  // Separate bind mounts can have equal st_dev while linkat returns EXDEV.
  // Without mount identity, stage at the destination conservatively.
#ifdef STATX_MNT_ID
  struct statx left_stat{}, right_stat{};
  if (::statx(AT_FDCWD, left.c_str(), AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &left_stat) == 0 &&
      ::statx(AT_FDCWD, right.c_str(), AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &right_stat) == 0 &&
      (left_stat.stx_mask & STATX_MNT_ID) && (right_stat.stx_mask & STATX_MNT_ID)) {
    return left_stat.stx_mnt_id == right_stat.stx_mnt_id;
  }
#endif
  return false;
}
YAML::Node identity(const fs::path& path) {
  const auto st = info(path);
  if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
    fail("Unexpected file type: " + path.string());
  YAML::Node result;
  result["device"] = uint64_t(st.st_dev);
  result["inode"] = uint64_t(st.st_ino);
  result["size"] = int64_t(st.st_size);
  result["mtime"] = int64_t(st.st_mtim.tv_sec);
  result["mtime_ns"] = int64_t(st.st_mtim.tv_nsec);
  result["directory"] = bool(S_ISDIR(st.st_mode));
  return result;
}
bool matches(const fs::path& path, const YAML::Node& expected, bool contents = false) {
  struct stat st{};
  if (!expected || ::lstat(path.c_str(), &st) != 0)
    return false;
  return uint64_t(st.st_dev) == expected["device"].as<uint64_t>() &&
      uint64_t(st.st_ino) == expected["inode"].as<uint64_t>() &&
      (!contents ||
       (S_ISREG(st.st_mode) && int64_t(st.st_size) == expected["size"].as<int64_t>() &&
        int64_t(st.st_mtim.tv_sec) == expected["mtime"].as<int64_t>() &&
        int64_t(st.st_mtim.tv_nsec) == expected["mtime_ns"].as<int64_t>()));
}
bool entry_exists(const fs::path& path) {
  struct stat st{};
  if (::lstat(path.c_str(), &st) == 0)
    return true;
  if (errno != ENOENT)
    fail("Cannot inspect " + path.string() + ": " + std::strerror(errno));
  return false;
}
void write_all(int fd, const std::string& text) {
  size_t offset = 0;
  while (offset < text.size()) {
    ssize_t written = ::write(fd, text.data() + offset, text.size() - offset);
    if (written < 0 && errno == EINTR)
      continue;
    check(written > 0, "Cannot write publication state");
    offset += written;
  }
}
void save(const fs::path& path, const YAML::Node& node) {
  const fs::path temporary = path.string() + ".next";
  Fd fd(::open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600));
  check(fd.value >= 0, "Cannot open publication state " + temporary.string());
  YAML::Emitter emitter;
  emitter << node;
  if (!emitter.good())
    fail("Cannot serialize publication state");
  write_all(fd.value, std::string(emitter.c_str()) + "\n");
  check(::fsync(fd.value) == 0, "Cannot synchronize publication state");
  check(::rename(temporary.c_str(), path.c_str()) == 0, "Cannot commit publication state");
  sync_directory(path.parent_path());
}
YAML::Node load(const fs::path& path) {
  const auto st = info(path);
  if (!S_ISREG(st.st_mode) || st.st_size > 16 * 1024 * 1024 || st.st_uid != ::geteuid())
    fail("Invalid publication manifest: " + path.string());
  auto node = YAML::LoadFile(path.string());
  if (node["schema"].as<int>(0) != 1)
    fail("Unsupported publication manifest");
  return node;
}
void private_directory(const fs::path& path) {
  if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
    fail("Cannot create " + path.string() + ": " + std::strerror(errno));
  const auto st = info(path);
  if (!S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 0077))
    fail("Run directory must be private and owned by the current user: " + path.string());
  sync_directory(path.parent_path());
}
struct ManifestLock {
  Fd fd;
  explicit ManifestLock(const fs::path& manifest)
      : fd(::open(
            (manifest.parent_path() / ".manifest.lock").c_str(),
            O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
            0600)) {
    check(fd.value >= 0 && ::flock(fd.value, LOCK_EX) == 0, "Cannot lock run manifest");
  }
};
struct GameLock {
  Fd fd;
  explicit GameLock(const fs::path& game)
      : fd(::open((game / ".hm-output-publication.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600)) {
    check(fd.value >= 0 && ::flock(fd.value, LOCK_EX | LOCK_NB) == 0, "Game publication is busy");
  }
};
void save_run(YAML::Node& run) {
  save(fs::path(run["work"].as<std::string>()) / "run.yaml", run);
}
void checkpoint(YAML::Node& run) {
  // The receipt is the recovery authority once publication starts, including
  // after run.yaml and its directory have been retired.
  save(ReceiptPath(run["game_dir"].as<std::string>(), run["id"].as<std::string>()), run);
}
void verify_binding(const YAML::Node& binding, const YAML::Node& run) {
  for (const char* key : {"id", "game_dir", "game_id", "work"}) {
    if (binding[key].as<std::string>() != run[key].as<std::string>())
      fail("Run manifest does not match its admission binding");
  }
  for (const char* key : {"device", "inode"}) {
    if (binding["work_identity"][key].as<uint64_t>() != run["work_identity"][key].as<uint64_t>())
      fail("Run working identity does not match its admission binding");
  }
}
YAML::Node load_admitted_run(const YAML::Node& binding) {
  const fs::path work = binding["work"].as<std::string>();
  if (!matches(work, binding["work_identity"]))
    fail("Admitted working directory is missing or replaced: " + work.string());
  auto run = load(work / "run.yaml");
  verify_binding(binding, run);
  return run;
}
void bind_admission(const YAML::Node& run) {
  // Keep only immutable location/identity here. Pipeline state and the observed
  // runner exit remain in run.yaml until publication replaces this locator.
  YAML::Node binding;
  binding["schema"] = 1;
  binding["state"] = "admitted";
  for (const char* key : {"id", "game_dir", "game_id", "work", "work_identity"})
    binding[key] = YAML::Clone(run[key]);
  checkpoint(binding);
}
void snapshot_tree(const fs::path& root, YAML::Node entries, const std::set<std::string>& excluded = {}) {
  for (const auto& entry : fs::directory_iterator(root)) {
    if (excluded.count(entry.path().filename().string()))
      continue;
    YAML::Node item;
    item["path"] = entry.path().string();
    item["identity"] = identity(entry.path());
    if (item["identity"]["directory"].as<bool>())
      snapshot_tree(entry.path(), entries);
    entries.push_back(item);
  }
}
volatile sig_atomic_t interrupted = 0;
volatile sig_atomic_t child_pid = 0;
void forward_signal(int signal) {
  interrupted = signal;
  const pid_t pid = child_pid;
  if (pid > 0)
    ::kill(-pid, signal);
}
class Signals {
 public:
  Signals() {
    interrupted = 0;
    struct sigaction action{};
    action.sa_handler = forward_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, &old_int_);
    sigaction(SIGTERM, &action, &old_term_);
  }
  ~Signals() {
    sigaction(SIGINT, &old_int_, nullptr);
    sigaction(SIGTERM, &old_term_, nullptr);
  }

 private:
  struct sigaction old_int_{}, old_term_{};
};
int execute(const std::vector<std::string>& arguments, const fs::path& log = {}, int inherited_fd = -1) {
  if (arguments.empty())
    fail("Missing child command");
  if (interrupted)
    fail("Job interrupted");
  Fd output(log.empty() ? -1 : ::open(log.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600));
  if (!log.empty())
    check(output.value >= 0, "Cannot open job log");
  std::vector<char*> argv;
  for (const auto& argument : arguments)
    argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);
  const pid_t parent = ::getpid();
  const pid_t pid = ::fork();
  check(pid >= 0, "Cannot start child process");
  if (pid == 0) {
    ::setpgid(0, 0);
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (::getppid() != parent)
      ::_exit(125);
    ::signal(SIGINT, SIG_DFL);
    ::signal(SIGTERM, SIG_DFL);
    if (output.value >= 0 && (::dup2(output.value, STDOUT_FILENO) < 0 || ::dup2(output.value, STDERR_FILENO) < 0))
      ::_exit(126);
    if (inherited_fd >= 0 && ::fcntl(inherited_fd, F_SETFD, 0) < 0)
      ::_exit(126);
    ::execvp(argv[0], argv.data());
    ::_exit(127);
  }
  child_pid = pid;
  ::setpgid(pid, pid);
  if (interrupted)
    ::kill(-pid, interrupted);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0)
    if (errno != EINTR)
      fail("Cannot wait for child process");
  child_pid = 0;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
void message(const Progress& progress, const std::string& text) {
  if (progress)
    progress(text);
}
void fault(const char* point) {
  // Deterministic process interruption tests exercise the on-disk recovery
  // protocol, not an alternative implementation of publication.
  const char* requested = ::getenv("HSTREAM_TEST_PUBLICATION_STOP");
  if (requested && std::string(requested) == point)
    fail(std::string("Injected interruption: ") + point);
}
std::string basename(const std::string& game, const std::string& kind, uint64_t generation) {
  const std::string suffix = "-" + std::to_string(generation);
  if (kind == "telemetry")
    return hm::recording::TelemetryDatabaseStem(game) + suffix + ".db";
  const std::string route = kind == "program" ? "tracking" : kind == "program-4k" ? "program_4k" : "stitched";
  return game + "-" + route + "_output-with-audio" + suffix + ".mp4";
}
uint64_t next_generation(const fs::path& game) {
  uint64_t result = 1;
  const std::regex version(".*-([0-9]+)\\.(mp4|db)(\\.hstream-pin)?");
  for (const auto& entry : fs::directory_iterator(game)) {
    std::smatch match;
    const std::string name = entry.path().filename().string();
    if (name.rfind("hstream-job-", 0) == 0 && entry.path().extension() == ".yaml") {
      const auto reserved = load(entry.path())["generation"].as<uint64_t>(0);
      if (reserved == std::numeric_limits<uint64_t>::max())
        fail("Output generation overflow");
      result = std::max(result, reserved + 1);
    }
    if (const auto telemetry = hm::recording::TelemetryDatabaseGeneration(name)) {
      if (*telemetry == std::numeric_limits<uint64_t>::max())
        fail("Output generation overflow");
      result = std::max(result, *telemetry + 1);
    }
    if (std::regex_match(name, match, version)) {
      const uint64_t value = std::stoull(match[1]);
      if (value == std::numeric_limits<uint64_t>::max())
        fail("Output generation overflow");
      result = std::max(result, value + 1);
    }
  }
  return result;
}
void verify_destinations(const YAML::Node& run) {
  for (auto artifact : run["artifacts"]) {
    if (!artifact["published"].as<bool>(false) ||
        !matches(artifact["destination"].as<std::string>(), artifact["prepared_identity"], true))
      fail("Published output is missing or changed: " + artifact["destination"].as<std::string>());
  }
}
void cleanup(YAML::Node& run, const Progress& progress, Fd& work_lock, std::vector<std::unique_ptr<Fd>>& owner_locks) {
  verify_destinations(run);
  // The game lock excludes new admissions throughout cleanup. Drop these
  // handles before recovering an interrupted deletion of a lock file: NFS
  // otherwise keeps an open, unlinked lock as a tombstone in its quarantine.
  work_lock.close();
  owner_locks.clear();
  ResumeCleanup(run, [&] { checkpoint(run); });
  const auto remove = [&](const fs::path& path, const YAML::Node& expected, bool contents = false) {
    CleanupOwned(path, expected, contents, run, [&] { checkpoint(run); });
  };
  const fs::path work = run["work"].as<std::string>();
  message(progress, "Removing completed run's working files");
  for (auto artifact : run["artifacts"]) {
    if (artifact["source"].IsDefined())
      remove(artifact["source"].as<std::string>(), artifact["source_identity"], true);
    if (artifact["prepared"].IsDefined())
      remove(artifact["prepared"].as<std::string>(), artifact["prepared_identity"], true);
  }
  fault("during-cleanup");
  // Explicitly registered identities take precedence over the later workspace
  // inventory, which must not adopt a replacement of an earlier generation.
  for (auto output : run["outputs"]) {
    if (output["retired"].as<bool>(false))
      remove(output["path"].as<std::string>(), output["identity"], true);
  }
  if (run["owned_scratch"]) {
    for (auto item : run["owned_scratch"])
      remove(item["path"].as<std::string>(), item["identity"]);
  }
  if (run["scratch"]) {
    for (auto item : run["scratch"])
      remove(item["path"].as<std::string>(), item["identity"]);
  }
  for (auto output : run["outputs"]) {
    if (output["owner_lock_identity"])
      remove(output["path"].as<std::string>() + ".hstream-owner-lock", output["owner_lock_identity"]);
  }
  if (run["launch_scratch"]) {
    for (auto item : run["launch_scratch"])
      remove(item["path"].as<std::string>(), item["identity"]);
  }
  const fs::path stage = run["stage"].as<std::string>();
  remove(stage, run["stage_identity"]);
  for (auto artifact : run["artifacts"]) {
    verify_destinations(run);
    remove(artifact["destination"].as<std::string>() + ".hstream-pin", artifact["prepared_identity"], true);
  }
  if (entry_exists(work)) {
    if (!matches(work, run["work_identity"]))
      fail("Working directory was replaced");
    for (auto item : run["metadata"])
      remove(item["path"].as<std::string>(), item["identity"], true);
    remove(work, run["work_identity"]);
  }
  fault("after-cleanup");
  run["state"] = "complete";
  checkpoint(run);
}
} // namespace

void ValidateRunId(const std::string& id) {
  if (!std::regex_match(id, std::regex("[a-zA-Z0-9_-]{8,100}")))
    fail("Invalid run ID");
}
fs::path ReceiptPath(const fs::path& game, const std::string& id) {
  ValidateRunId(id);
  return game / ("hstream-job-" + id + ".yaml");
}
fs::path EnvironmentManifest() {
  const char* value = ::getenv("HSTREAM_JOB_MANIFEST");
  return value && *value ? fs::path(value) : fs::path();
}
void BeginRun(const fs::path& manifest, bool telemetry_required, bool telemetry_only) {
  if (telemetry_only && !telemetry_required)
    fail("Telemetry-only publication requires telemetry capture");
  ManifestLock lock(manifest);
  auto run = load(manifest);
  if (run["state"].as<std::string>() != "created")
    fail("Run has already started");
  run["state"] = "running";
  run["telemetry_required"] = telemetry_required;
  run["telemetry_only"] = telemetry_only;
  save_run(run);
}
bool PipelineCompleted(const fs::path& manifest) {
  return load(manifest)["state"].as<std::string>() == "encoded";
}
fs::path EnvironmentWorkingDirectory() {
  const auto path = EnvironmentManifest();
  return path.empty() ? fs::path() : path.parent_path();
}
void ValidatePipelineStart(bool supports_telemetry) {
  const auto manifest = EnvironmentManifest();
  if (manifest.empty())
    return;
  ManifestLock lock(manifest);
  const auto run = load(manifest);
  if (run["state"].as<std::string>() != "running")
    fail("Managed pipeline requires a running manifest");
  if (!supports_telemetry && run["telemetry_required"].as<bool>(false))
    fail("This pipeline does not support the requested telemetry capture");
}
static fs::path create_run(const fs::path& game, const fs::path& output_root, const std::string& id) {
  ValidateRunId(id);
  const fs::path logical_game = fs::absolute(game).lexically_normal();
  const std::string game_id = logical_game.filename();
  hm::recording::TelemetryDatabaseStem(game_id);
  const fs::path destination = fs::canonical(logical_game);
  const auto receipt = ReceiptPath(destination, id);
  if (entry_exists(receipt)) {
    const auto binding = load(receipt);
    if (binding["id"].as<std::string>() != id || binding["game_dir"].as<std::string>() != destination.string() ||
        binding["game_id"].as<std::string>() != game_id)
      fail("Run identity does not match requested game");
    if (binding["state"].as<std::string>() != "admitted")
      fail("Run already has a publication receipt; resume it instead");
    const auto run = load_admitted_run(binding);
    return fs::path(run["work"].as<std::string>()) / "run.yaml";
  }
  if (output_root.empty())
    fail("A new run requires a working output root");
  const fs::path parent = fs::absolute(output_root) / game_id / "runs";
  fs::create_directories(parent);
  const fs::path work = fs::canonical(parent) / id;
  private_directory(work);
  const fs::path manifest = work / "run.yaml";
  ManifestLock initialization(manifest);
  if (entry_exists(manifest)) {
    const auto run = load(manifest);
    if (run["id"].as<std::string>() != id || run["game_dir"].as<std::string>() != destination.string() ||
        run["game_id"].as<std::string>() != game_id || !matches(work, run["work_identity"]))
      fail("Run identity does not match requested game");
    bind_admission(run);
    return manifest;
  }
  for (const auto& entry : fs::directory_iterator(work)) {
    if (entry.path().filename() != ".manifest.lock")
      fail("Refusing to adopt a nonempty working directory");
  }
  YAML::Node run;
  run["schema"] = 1;
  run["id"] = id;
  run["game_dir"] = destination.string();
  run["game_id"] = game_id;
  run["work"] = work.string();
  run["work_identity"] = identity(work);
  run["state"] = "created";
  run["outputs"] = YAML::Node(YAML::NodeType::Sequence);
  save(manifest, run);
  bind_admission(run);
  return manifest;
}
fs::path CreateRun(const fs::path& game, const fs::path& output_root, const std::string& id) {
  GameLock admission(fs::canonical(game));
  return create_run(game, output_root, id);
}
void RegisterOutput(const fs::path& path, const std::string& kind, bool hevc, int expected_fd) {
  const auto manifest = EnvironmentManifest();
  if (manifest.empty())
    return;
  ManifestLock lock(manifest);
  auto run = load(manifest);
  if (run["state"].as<std::string>() != "running")
    fail("Cannot add output to a closed run");
  const auto absolute = fs::absolute(path).lexically_normal();
  const auto st = info(absolute);
  if (!S_ISREG(st.st_mode))
    fail("Run output is not a regular file");
  const auto owned_identity = identity(absolute);
  if (expected_fd >= 0) {
    struct stat expected{};
    check(::fstat(expected_fd, &expected) == 0, "Cannot inspect owned output descriptor");
    if (uint64_t(expected.st_dev) != owned_identity["device"].as<uint64_t>() ||
        uint64_t(expected.st_ino) != owned_identity["inode"].as<uint64_t>())
      fail("Output file was replaced before registration");
  }
  for (auto output : run["outputs"]) {
    if (output["path"].as<std::string>() == absolute.string()) {
      if (!matches(absolute, output["identity"]))
        fail("Working output was replaced");
      return;
    }
  }
  YAML::Node output;
  output["path"] = absolute.string();
  output["kind"] = kind;
  output["hevc"] = hevc;
  output["identity"] = owned_identity;
  const fs::path owner_lock = absolute.string() + ".hstream-owner-lock";
  if (entry_exists(owner_lock))
    output["owner_lock_identity"] = identity(owner_lock);
  run["outputs"].push_back(output);
  save_run(run);
}
void RetireOutput(const fs::path& path, bool require_empty_telemetry) {
  const auto manifest = EnvironmentManifest();
  if (manifest.empty() || path.empty())
    return;
  ManifestLock lock(manifest);
  auto run = load(manifest);
  if (run["state"].as<std::string>() != "running")
    fail("Cannot retire an output of a closed run");
  const auto absolute = fs::absolute(path).lexically_normal();
  for (auto output : run["outputs"]) {
    if (output["path"].as<std::string>() != absolute.string() || output["retired"].as<bool>(false))
      continue;
    if (!matches(absolute, output["identity"]))
      fail("Retired working output was replaced");
    if (require_empty_telemetry) {
      if (output["kind"].as<std::string>() != "telemetry")
        fail("Only empty telemetry can be retired during caps negotiation");
      hm::recording::Database database(absolute.string());
      database.Validate();
      hm::recording::Statement samples(database.get(), "SELECT coalesce(sum(sample_count),0) FROM runs");
      if (!samples.Next() || samples.Int(0) != 0)
        fail("Cannot retire telemetry containing recorded samples during caps negotiation");
    }
    output["identity"] = identity(absolute);
    output["retired"] = true;
    save_run(run);
    return;
  }
  fail("Cannot retire an unregistered output: " + absolute.string());
}
void RegisterScratch(const fs::path& path, const fs::path& explicit_manifest, int expected_fd) {
  const auto manifest = explicit_manifest.empty() ? EnvironmentManifest() : explicit_manifest;
  if (manifest.empty())
    return;
  ManifestLock lock(manifest);
  auto run = load(manifest);
  const auto state = run["state"].as<std::string>();
  if (state != "running" && state != "encoded")
    fail("Cannot register scratch after publication starts");
  const auto absolute = fs::absolute(path).lexically_normal();
  if (!run["owned_scratch"])
    run["owned_scratch"] = YAML::Node(YAML::NodeType::Sequence);
  YAML::Node item;
  item["path"] = absolute.string();
  item["identity"] = identity(absolute);
  if (expected_fd >= 0) {
    struct stat expected{};
    check(::fstat(expected_fd, &expected) == 0, "Cannot inspect owned scratch descriptor");
    if (item["identity"]["device"].as<uint64_t>() != uint64_t(expected.st_dev) ||
        item["identity"]["inode"].as<uint64_t>() != uint64_t(expected.st_ino))
      fail("Scratch file was replaced during registration");
  }
  if (item["identity"]["directory"].as<bool>())
    snapshot_tree(absolute, run["owned_scratch"]);
  run["owned_scratch"].push_back(item);
  save_run(run);
}
void RequireTelemetry(bool required) {
  const auto manifest = EnvironmentManifest();
  if (manifest.empty())
    return;
  ManifestLock lock(manifest);
  auto run = load(manifest);
  if (run["telemetry_only"].as<bool>(false) && !required)
    fail("Telemetry-only publication requires telemetry capture");
  run["telemetry_required"] = required;
  save_run(run);
}
void CompletePipeline(bool success, int64_t start_time_ms) {
  const auto manifest = EnvironmentManifest();
  if (manifest.empty())
    return;
  ManifestLock lock(manifest);
  auto run = load(manifest);
  if (run["state"].as<std::string>() != "running")
    fail("Invalid pipeline completion state");
  if (success) {
    bool video = false, telemetry = false;
    for (auto output : run["outputs"]) {
      if (output["retired"].as<bool>(false))
        continue;
      const fs::path path = output["path"].as<std::string>();
      if (!matches(path, output["identity"]))
        fail("Working output changed before pipeline completion");
      output["identity"] = identity(path);
      if (output["identity"]["size"].as<int64_t>() <= 0)
        fail("Empty pipeline output: " + path.string());
      telemetry |= output["kind"].as<std::string>() == "telemetry";
      video |= output["kind"].as<std::string>() != "telemetry";
    }
    const bool telemetry_only = run["telemetry_only"].as<bool>(false);
    if ((telemetry_only ? (!telemetry || video) : !video) || (run["telemetry_required"].as<bool>(false) && !telemetry))
      fail("Pipeline did not produce all required outputs");
  }
  run["start_time_ms"] = start_time_ms;
  run["state"] = success ? "encoded" : "failed";
  save_run(run);
}
std::vector<std::string> RemuxArguments(const std::string& input, const std::string& output, bool hevc) {
  std::vector<std::string> result = {
      "-hide_banner",
      "-nostdin",
      "-n",
      "-progress",
      "pipe:1",
      "-nostats",
      "-i",
      input,
      "-map",
      "0:v:0",
      "-map",
      "0:a?",
      "-map_metadata",
      "0",
      "-c",
      "copy",
      "-movflags",
      "+faststart"};
  if (hevc) {
    result.push_back("-tag:v");
    result.push_back("hvc1");
  }
  result.push_back(output);
  return result;
}
void CopyTelemetrySnapshot(const fs::path& source, const fs::path& destination, const std::string& game_id) {
  hm::recording::Database input(source.string());
  input.Validate();
  input.Exec("BEGIN");
  hm::recording::Statement runs(input.get(), "SELECT count(*),sum(completed),sum(sample_count) FROM runs");
  if (!runs.Next() || !runs.Int(0) || runs.Int(0) != runs.Int(1) || runs.Int(2) <= 0)
    fail("Only completed telemetry recordings can be published");
  hm::recording::Statement games(input.get(), "SELECT DISTINCT game_id FROM runs");
  if (!games.Next() || (!game_id.empty() && games.Text(0) != game_id) || games.Next())
    fail("Telemetry game does not match publication");
  hm::recording::Database output(destination.string(), true);
  sqlite3_backup* backup = sqlite3_backup_init(output.get(), "main", input.get(), "main");
  if (!backup)
    fail("Cannot initialize database snapshot");
  int code;
  do {
    code = sqlite3_backup_step(backup, 256);
  } while (code == SQLITE_OK);
  const int finished = sqlite3_backup_finish(backup);
  if (code != SQLITE_DONE || finished != SQLITE_OK)
    fail("Database snapshot failed");
  output.Validate();
  hm::recording::Statement check_db(output.get(), "PRAGMA quick_check");
  if (!check_db.Next() || check_db.Text(0) != "ok")
    fail("Database snapshot failed integrity check");
}

std::vector<PublishedOutput> Finalize(const fs::path& manifest_or_receipt, const Progress& progress) {
  auto run = load(manifest_or_receipt);
  const fs::path game = run["game_dir"].as<std::string>();
  const auto receipt = ReceiptPath(game, run["id"].as<std::string>());
  GameLock game_lock(game);
  if (entry_exists(receipt)) {
    const auto saved = load(receipt);
    verify_binding(saved, run);
    run = saved;
  }
  const fs::path work = run["work"].as<std::string>();
  const auto initial_state = run["state"].as<std::string>();
  const bool require_work_lock = initial_state != "published" && initial_state != "complete";
  if (require_work_lock && !matches(work, run["work_identity"]))
    fail("Working directory is missing or replaced: " + work.string());
  const auto lock_path = work / ".run.lock";
  const bool open_work_lock = entry_exists(work) && (require_work_lock || entry_exists(lock_path));
  Fd work_lock(
      open_work_lock
          ? ::open(lock_path.c_str(), (require_work_lock ? O_CREAT : 0) | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600)
          : -1);
  if (open_work_lock)
    check(work_lock.value >= 0 && ::flock(work_lock.value, LOCK_EX | LOCK_NB) == 0, "Run is still active");
  if (initial_state == "admitted")
    run = load_admitted_run(run);
  std::vector<std::unique_ptr<Fd>> owner_locks;
  for (auto output : run["outputs"]) {
    if (!output["owner_lock_identity"])
      continue;
    const fs::path owner = output["path"].as<std::string>() + ".hstream-owner-lock";
    if (!entry_exists(owner)) {
      if (entry_exists(output["path"].as<std::string>()))
        fail("Working archive owner lock disappeared");
      continue;
    }
    if (!matches(owner, output["owner_lock_identity"]))
      fail("Archive owner lock was replaced");
    auto fd = std::make_unique<Fd>(::open(owner.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW));
    check(fd->value >= 0 && ::flock(fd->value, LOCK_EX | LOCK_NB) == 0, "Working archive is still owned by a producer");
    owner_locks.push_back(std::move(fd));
  }
  bool extra_scratch = false;
  for (const char* key : {"HSTREAM_JOB_PLUGIN_STAGE", "HSTREAM_JOB_LIBRARY_STAGE"}) {
    const char* value = ::getenv(key);
    if (!value || !*value || !entry_exists(value))
      continue;
    const fs::path path(value);
    private_directory(path);
    if (!run["launch_scratch"])
      run["launch_scratch"] = YAML::Node(YAML::NodeType::Sequence);
    YAML::Node discovered(YAML::NodeType::Sequence);
    snapshot_tree(path, discovered);
    YAML::Node item;
    item["path"] = path.string();
    item["identity"] = identity(path);
    discovered.push_back(item);
    for (auto entry : discovered) {
      bool known = false;
      for (auto previous : run["launch_scratch"]) {
        if (previous["path"].as<std::string>() != entry["path"].as<std::string>())
          continue;
        if (!matches(entry["path"].as<std::string>(), previous["identity"]))
          fail("Launcher scratch was replaced");
        known = true;
        break;
      }
      if (!known) {
        run["launch_scratch"].push_back(entry);
        extra_scratch = true;
      }
    }
  }
  std::vector<PublishedOutput> result;
  auto state = run["state"].as<std::string>();
  if (run["supervised"].as<bool>(false) && !run["runner_succeeded"].as<bool>(false))
    fail("Runner success was not recorded; working data retained");
  if ((state == "complete" || state == "published") && extra_scratch) {
    verify_destinations(run);
    run["state"] = state = "published";
    checkpoint(run);
  }
  if (state == "publishing" && extra_scratch)
    checkpoint(run);
  if (state == "complete" || state == "published") {
    verify_destinations(run);
    if (state == "published")
      cleanup(run, progress, work_lock, owner_locks);
    for (auto artifact : run["artifacts"])
      result.push_back({artifact["destination"].as<std::string>(), artifact["kind"].as<std::string>()});
    return result;
  }
  if (state != "encoded" && state != "publishing")
    fail("Run has not completed encoding successfully");
  if (!matches(work, run["work_identity"]))
    fail("Working directory was replaced");
  if (state == "encoded") {
    run["generation"] = next_generation(game);
    const bool same_filesystem = same_mount(game, work);
    run["stage"] =
        (same_filesystem ? work / "publication" : game / (".hstream-finalize-" + run["id"].as<std::string>())).string();
    run["state"] = "publishing";
    run["artifacts"] = YAML::Node(YAML::NodeType::Sequence);
    run["scratch"] = YAML::Node(YAML::NodeType::Sequence);
    run["metadata"] = YAML::Node(YAML::NodeType::Sequence);
    for (const char* name : {"run.yaml.next", ".manifest.lock", ".run.lock", "run.yaml"}) {
      const auto path = work / name;
      if (!entry_exists(path))
        continue;
      if (!S_ISREG(info(path).st_mode))
        fail("Unexpected run metadata file: " + path.string());
      YAML::Node item;
      item["path"] = path.string();
      item["identity"] = identity(path);
      run["metadata"].push_back(item);
    }
    snapshot_tree(work, run["scratch"], {"run.yaml", "run.yaml.next", ".manifest.lock", ".run.lock"});
    std::set<std::string> kinds;
    for (auto output : run["outputs"]) {
      if (output["retired"].as<bool>(false))
        continue;
      const std::string kind = output["kind"].as<std::string>();
      if (!kinds.insert(kind).second)
        fail("Multiple outputs of one kind require separate jobs");
      YAML::Node artifact;
      artifact["source"] = output["path"];
      artifact["source_identity"] = output["identity"];
      artifact["kind"] = kind;
      artifact["hevc"] = output["hevc"];
      artifact["destination"] =
          (game / basename(run["game_id"].as<std::string>(), kind, run["generation"].as<uint64_t>())).string();
      run["artifacts"].push_back(artifact);
      if (kind != "telemetry") {
        YAML::Node sidecar;
        sidecar["kind"] = "sidecar";
        sidecar["video_kind"] = kind;
        sidecar["destination"] = artifact["destination"].as<std::string>() + ".hstream-archive.json";
        run["artifacts"].push_back(sidecar);
      }
    }
    checkpoint(run);
  }
  const fs::path stage = run["stage"].as<std::string>();
  if (!run["stage_identity"]) {
    private_directory(stage);
    if (!fs::is_empty(stage))
      fail("Unrecognized publication staging directory");
    run["stage_identity"] = identity(stage);
    checkpoint(run);
  } else if (!matches(stage, run["stage_identity"]))
    fail("Publication staging directory was replaced");
  size_t index = 0;
  for (auto artifact : run["artifacts"]) {
    if (interrupted)
      fail("Publication interrupted");
    const fs::path destination = artifact["destination"].as<std::string>();
    const fs::path prepared = stage / (std::to_string(index++) + ".part");
    if (artifact["prepared_identity"]) {
      if (entry_exists(destination)) {
        if (!matches(destination, artifact["prepared_identity"], true))
          fail("Destination was replaced: " + destination.string());
        artifact["published"] = true;
        checkpoint(run);
        continue;
      }
      if (!matches(prepared, artifact["prepared_identity"], true))
        fail("Prepared output was replaced");
    } else {
      // An interrupted producer can leave its incomplete file in this private,
      // identity-checked staging directory. It has never been published.
      if (entry_exists(prepared)) {
        if (!S_ISREG(info(prepared).st_mode))
          fail("Unexpected staging entry");
        check(::unlink(prepared.c_str()) == 0, "Cannot discard interrupted preparation");
      }
      const auto kind = artifact["kind"].as<std::string>();
      if (kind == "sidecar") {
        // Game directory names may contain control characters. YAML's \xNN
        // escapes are not valid JSON, so quote the sidecar strings explicitly.
        const auto quote = [](const std::string& value) {
          static constexpr char hex[] = "0123456789abcdef";
          std::string result = "\"";
          for (unsigned char c : value) {
            if (c == '"' || c == '\\') {
              result += '\\';
              result += c;
            } else if (c < 0x20) {
              result += "\\u00";
              result += hex[c >> 4];
              result += hex[c & 15];
            } else {
              result += c;
            }
          }
          return result + '"';
        };
        const std::string video_kind = artifact["video_kind"].as<std::string>();
        const std::string data = "{\"schema\":1,\"game_id\":" + quote(run["game_id"].as<std::string>()) +
            ",\"kind\":" + quote(video_kind == "program-4k" ? "program_4k" : video_kind) +
            ",\"start_time_ms\":" + std::to_string(run["start_time_ms"].as<int64_t>(0)) + "}\n";
        Fd fd(::open(prepared.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
        check(fd.value >= 0, "Cannot create archive sidecar");
        write_all(fd.value, data);
      } else {
        const fs::path source = artifact["source"].as<std::string>();
        if (!matches(source, artifact["source_identity"], true))
          fail("Working output was replaced: " + source.string());
        message(progress, "Preparing " + destination.filename().string());
        if (kind == "telemetry") {
          Fd database_file(::open(prepared.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600));
          check(database_file.value >= 0, "Cannot create telemetry staging file");
          CopyTelemetrySnapshot(source, prepared, run["game_id"].as<std::string>());
        } else {
          Fd source_fd(::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
          check(source_fd.value >= 0, "Cannot open working video");
          struct stat pinned{};
          check(::fstat(source_fd.value, &pinned) == 0, "Cannot inspect working video");
          if (uint64_t(pinned.st_dev) != artifact["source_identity"]["device"].as<uint64_t>() ||
              uint64_t(pinned.st_ino) != artifact["source_identity"]["inode"].as<uint64_t>())
            fail("Working video was replaced while opening");
          const char* override = ::getenv("HSTREAM_FFMPEG");
          if (!override || !*override)
            override = ::getenv("HSTREAM_UI_FFMPEG");
          std::vector<std::string> arguments = {override && *override ? override : "ffmpeg"};
          auto remux = RemuxArguments(
              "/proc/self/fd/" + std::to_string(source_fd.value), prepared.string(), artifact["hevc"].as<bool>(false));
          // The staging filename is private protocol data, not an extension.
          remux.insert(remux.end() - 1, {"-f", "mp4"});
          arguments.insert(arguments.end(), remux.begin(), remux.end());
          if (execute(arguments, {}, source_fd.value) != 0)
            fail("ffmpeg did not finish; working outputs retained");
          if (!matches(source, artifact["source_identity"], true))
            fail("Working video changed during remux");
        }
      }
      Fd fd(::open(prepared.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
      check(fd.value >= 0 && ::fsync(fd.value) == 0, "Cannot synchronize prepared output");
      if (info(prepared).st_size <= 0)
        fail("Empty prepared output");
      sync_directory(stage);
      artifact["prepared"] = prepared.string();
      artifact["prepared_identity"] = identity(prepared);
      checkpoint(run);
      fault("after-prepare");
    }
    Fd prepared_fd(::open(prepared.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    check(prepared_fd.value >= 0, "Cannot pin prepared output");
    struct stat pinned{};
    check(::fstat(prepared_fd.value, &pinned) == 0, "Cannot inspect prepared output");
    if (uint64_t(pinned.st_dev) != artifact["prepared_identity"]["device"].as<uint64_t>() ||
        uint64_t(pinned.st_ino) != artifact["prepared_identity"]["inode"].as<uint64_t>())
      fail("Prepared output was replaced while opening");
    const fs::path guard = destination.string() + ".hstream-pin";
    if (entry_exists(guard)) {
      if (!matches(guard, artifact["prepared_identity"], true))
        fail("Output reservation belongs to another file");
    } else {
      check(hm::link_pinned_file(prepared_fd.value, AT_FDCWD, guard.c_str()) == 0, "Cannot reserve published output");
      sync_directory(game);
    }
    check(
        hm::link_pinned_file(prepared_fd.value, AT_FDCWD, destination.c_str()) == 0,
        "Cannot publish without overwriting " + destination.string());
    sync_directory(game);
    fault("after-link");
    artifact["published"] = true;
    checkpoint(run);
    message(progress, "Published " + destination.string());
  }
  verify_destinations(run);
  run["state"] = "published";
  checkpoint(run);
  fault("after-publish");
  cleanup(run, progress, work_lock, owner_locks);
  for (auto artifact : run["artifacts"])
    result.push_back({artifact["destination"].as<std::string>(), artifact["kind"].as<std::string>()});
  return result;
}

int RunJob(
    const fs::path& game,
    const fs::path& output_root,
    const std::string& id,
    const std::vector<std::string>& command,
    const Progress& progress,
    bool telemetry_only) {
  Signals signals;
  YAML::Node launch_scratch(YAML::NodeType::Sequence);
  for (const char* key : {"HSTREAM_JOB_PLUGIN_STAGE", "HSTREAM_JOB_LIBRARY_STAGE"}) {
    const char* value = ::getenv(key);
    if (!value || !*value)
      continue;
    const fs::path path(value);
    private_directory(path);
    snapshot_tree(path, launch_scratch);
    YAML::Node item;
    item["path"] = path.string();
    item["identity"] = identity(path);
    launch_scratch.push_back(item);
  }
  // Publication owns the same stable game lock until its private directory is
  // retired. Select the receipt and acquire the run lock under that lock, so a
  // concurrent retry cannot recreate a completed run in the retirement gap.
  GameLock admission(fs::canonical(game));
  const auto receipt = ReceiptPath(fs::canonical(game), id);
  if (entry_exists(receipt) && load(receipt)["state"].as<std::string>() != "admitted") {
    admission.fd.close();
    Finalize(receipt, progress);
    return 0;
  }
  const auto manifest = create_run(game, output_root, id);
  const auto work = manifest.parent_path();
  Fd lock(::open((work / ".run.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
  check(lock.value >= 0 && ::flock(lock.value, LOCK_EX | LOCK_NB) == 0, "Job is already running");
  auto run = load(manifest);
  if (run["state"].as<std::string>() == "created") {
    run["state"] = "running";
    run["supervised"] = true;
    run["telemetry_only"] = telemetry_only;
    run["telemetry_required"] = telemetry_only;
    run["launch_scratch"] = launch_scratch;
    for (const auto& argument : command)
      run["command"].push_back(argument);
    save_run(run);
    admission.fd.close();
    ::setenv("HSTREAM_JOB_MANIFEST", manifest.c_str(), 1);
    ::setenv("HSTREAM_ARCHIVE_RUN_ID", id.c_str(), 1);
    ::setenv("HSTREAM_RUNTIME_CACHE_DIR", (work / "runtime").c_str(), 1);
    message(progress, "Encoding job; log: " + (work / "pipeline.log").string());
    const int code = execute(command, work / "pipeline.log");
    run = load(manifest);
    fault("after-runner-exit");
    const int outcome = interrupted ? 128 + interrupted : code;
    run["runner_exit"] = outcome;
    run["runner_succeeded"] = outcome == 0;
    if (outcome != 0)
      run["state"] = "failed";
    save_run(run);
    if (outcome != 0)
      return outcome;
    fault("after-runner-success");
  }
  if (run["state"].as<std::string>() != "encoded")
    fail("Encoding did not complete; working data retained at " + work.string());
  check(::flock(lock.value, LOCK_UN) == 0, "Cannot release run lock for publication");
  lock.close();
  admission.fd.close();
  Finalize(manifest, progress);
  return 0;
}
} // namespace hm::publication
