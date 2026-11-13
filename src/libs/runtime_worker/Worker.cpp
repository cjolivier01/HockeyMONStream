#include "hstream/src/libs/runtime_worker/Worker.h"
#include "hstream/src/libs/common/ProcessGuard.h"

#include <curl/curl.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace hm::runtime_worker {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr const char* kSchema = "hstream-runtime-worker-v1";
constexpr size_t kDocumentLimit = 4 * 1024 * 1024;
constexpr size_t kPipeLimit = 65536;
volatile sig_atomic_t interrupted = 0;
void Stop(int value) {
  interrupted = value;
}
void Require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Check(const absl::Status& status) {
  Require(status.ok(), status.ToString());
}
struct Fd {
  int value = -1;
  Fd() = default;
  explicit Fd(int descriptor) : value(descriptor) {
    Require(value >= 0, std::strerror(errno));
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
  Fd& operator=(Fd&& other) noexcept {
    Close();
    value = std::exchange(other.value, -1);
    return *this;
  }
  void Close() {
    if (value >= 0)
      ::close(std::exchange(value, -1));
  }
  ~Fd() {
    Close();
  }
};
std::string Nonce() {
  gchar* value = g_uuid_string_random();
  std::string result(value);
  g_free(value);
  result.erase(std::remove(result.begin(), result.end(), '-'), result.end());
  return result;
}
void WriteAll(int descriptor, const std::string& value) {
  size_t offset = 0;
  while (offset < value.size()) {
    const auto count = ::write(descriptor, value.data() + offset, value.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    Require(count > 0, "Cannot write protocol document");
    offset += count;
  }
}
std::string Read(const fs::path& path, size_t maximum = kDocumentLimit, bool snapshot = false) {
  Fd fd(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  struct stat info {};
  Require(
      ::fstat(fd.value, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 &&
          static_cast<uint64_t>(info.st_size) <= maximum,
      "Invalid bounded regular file: " + path.string());
  std::string result;
  std::array<char, 65536> buffer;
  for (;;) {
    const auto count = ::read(fd.value, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR)
      continue;
    Require(count >= 0, "Cannot read protocol document");
    if (!count)
      break;
    result.append(buffer.data(), count);
    Require(result.size() <= maximum, "Protocol document exceeds its bound");
  }
  struct stat after {
  }, named{};
  Require(
      ::fstat(fd.value, &after) == 0 && after.st_dev == info.st_dev && after.st_ino == info.st_ino &&
          after.st_size == info.st_size && after.st_mtim.tv_sec == info.st_mtim.tv_sec &&
          after.st_mtim.tv_nsec == info.st_mtim.tv_nsec &&
          (snapshot ||
           (::lstat(path.c_str(), &named) == 0 && after.st_ctim.tv_sec == info.st_ctim.tv_sec &&
            after.st_ctim.tv_nsec == info.st_ctim.tv_nsec && named.st_dev == info.st_dev &&
            named.st_ino == info.st_ino)),
      "Protocol file changed while reading");
  return result;
}
void Atomic(const fs::path& path, const Json& document) {
  const auto text = document.Dump();
  Require(text.size() <= kDocumentLimit, "Protocol response exceeds its bound");
  const auto temporary = path.parent_path() / ("." + path.filename().string() + "." + Nonce());
  try {
    Fd fd(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    WriteAll(fd.value, text);
    Require(
        ::fsync(fd.value) == 0 && ::rename(temporary.c_str(), path.c_str()) == 0, "Cannot publish protocol document");
    Fd parent(::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    Require(::fsync(parent.value) == 0, "Cannot synchronize protocol directory");
  } catch (...) {
    ::unlink(temporary.c_str());
    throw;
  }
}
void PrivateDirectory(const fs::path& path, bool create) {
  if (create && ::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
    throw std::runtime_error("Cannot create private directory: " + path.string());
  struct stat info {};
  Require(
      ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == ::geteuid() &&
          !(info.st_mode & 0077),
      "Runtime directory must be owned and private: " + path.string());
}
int64_t WallTimeMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}
std::string Host() {
  char host[256]{};
  Require(::gethostname(host, sizeof(host) - 1) == 0, "Cannot identify runtime host");
  return host;
}
Json Identity() {
  Json result;
  result.Set("host", Host());
  auto boot = Read("/proc/sys/kernel/random/boot_id", 128);
  while (!boot.empty() && boot.back() == '\n')
    boot.pop_back();
  result.Set("boot", boot);
  result.Set("pid", static_cast<int64_t>(::getpid()));
  // procfs reports zero st_size, so read the process stat through its stream.
  std::ifstream stream("/proc/self/stat");
  std::string line;
  Require(static_cast<bool>(std::getline(stream, line)), "Cannot read runtime process identity");
  std::istringstream fields(line.substr(line.rfind(')') + 2));
  std::string value;
  for (int index = 3; index <= 22; ++index)
    Require(static_cast<bool>(fields >> value), "Invalid runtime process identity");
  result.Set("start", static_cast<int64_t>(std::stoll(value)));
  struct utsname machine {};
  Require(::uname(&machine) == 0, "Cannot identify runtime architecture");
  result.Set("architecture", machine.machine);
  if (const char* node = ::getenv("SLURMD_NODENAME"))
    result.Set("node", node);
  else
    result.Set("node", Host());
  return result;
}
Json File(
    const fs::path& path,
    int64_t maximum,
    const std::function<void()>& checkpoint,
    Fd* retained = nullptr,
    std::string* contents = nullptr) {
  Fd fd(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  struct stat before {
  }, after{}, named{};
  Require(
      ::fstat(fd.value, &before) == 0 && S_ISREG(before.st_mode) && before.st_size >= 0 && before.st_size <= maximum,
      "File is not a bounded regular file: " + path.string());
  auto* checksum = g_checksum_new(G_CHECKSUM_SHA256);
  try {
    std::array<guchar, 1024 * 1024> buffer;
    int64_t total = 0;
    for (;;) {
      checkpoint();
      const auto count = ::read(fd.value, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR)
        continue;
      Require(count >= 0, "Cannot hash runtime file");
      if (!count)
        break;
      total += count;
      Require(total <= maximum, "File grew beyond its allowance");
      g_checksum_update(checksum, buffer.data(), count);
      if (contents)
        contents->append(reinterpret_cast<const char*>(buffer.data()), count);
    }
    Require(
        ::fstat(fd.value, &after) == 0 && ::lstat(path.c_str(), &named) == 0 && before.st_dev == after.st_dev &&
            before.st_ino == after.st_ino && before.st_size == after.st_size &&
            before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
            before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec &&
            named.st_dev == before.st_dev && named.st_ino == before.st_ino && total == before.st_size,
        "File identity changed while hashing");
    Json result;
    result.Set("path", path.string());
    result.Set("device", static_cast<int64_t>(before.st_dev));
    result.Set("inode", static_cast<int64_t>(before.st_ino));
    result.Set("size", static_cast<int64_t>(before.st_size));
    result.Set("mtime", static_cast<int64_t>(before.st_mtim.tv_sec));
    result.Set("mtime_ns", static_cast<int64_t>(before.st_mtim.tv_nsec));
    result.Set("ctime", static_cast<int64_t>(before.st_ctim.tv_sec));
    result.Set("ctime_ns", static_cast<int64_t>(before.st_ctim.tv_nsec));
    result.Set("sha256", g_checksum_get_string(checksum));
    g_checksum_free(checksum);
    if (retained)
      *retained = std::move(fd);
    return result;
  } catch (...) {
    g_checksum_free(checksum);
    throw;
  }
}
std::string Base64(const std::string& value) {
  gchar* encoded = g_base64_encode(reinterpret_cast<const guchar*>(value.data()), value.size());
  std::string result(encoded);
  g_free(encoded);
  return result;
}
std::string Unbase64(const std::string& value) {
  Require(
      value.size() <= 2 * kPipeLimit && std::regex_match(value, std::regex("[A-Za-z0-9+/]*={0,2}")),
      "Invalid bounded base64 input");
  gsize count = 0;
  guchar* decoded = g_base64_decode(value.c_str(), &count);
  std::string result(reinterpret_cast<char*>(decoded), count);
  g_free(decoded);
  Require(result.size() <= kPipeLimit && Base64(result) == value, "Invalid canonical base64 input");
  return result;
}
struct Task {
  std::string id, nonce, header, input;
  fs::path receipt;
  fs::path output, errors;
  Fd proof, pipe;
  pid_t pid = 0;
  std::optional<int> code;
  bool stopped = false;
  bool close_input = false;
  bool timed_out = false;
  Clock::time_point deadline;
};
class Worker {
 public:
  explicit Worker(const fs::path& request_path)
      : root_(fs::absolute(request_path).parent_path()), request_(Json::Parse(Read(request_path))) {
    Require(root_ == fs::canonical(root_), "Worker coordination root contains a symlink");
    PrivateDirectory(root_, false);
    Require(request_.Text("schema") == kSchema, "Unsupported runtime-worker protocol");
    id_ = request_.Text("id");
    token_ = request_.Text("token");
    Require(
        std::regex_match(id_, std::regex("[A-Za-z0-9_-]{1,128}")) &&
            std::regex_match(token_, std::regex("[a-f0-9]{32,128}")),
        "Invalid opaque session binding");
    workspace_ = fs::path(request_.Text("workspace"));
    Require(
        workspace_.is_absolute() && workspace_ == fs::canonical(workspace_) && root_.parent_path() == workspace_,
        "Coordination must be a direct child of its admitted workspace");
    PrivateDirectory(workspace_, false);
    const auto limits = request_.Get("limits");
    poll_ms_ = limits.Integer("poll_ms", 1, 1000);
    grace_ms_ = limits.Integer("retire_ms", poll_ms_, 600000);
    timeout_ms_ = limits.Integer("timeout_ms", 1, 30LL * 24 * 3600 * 1000);
    client_ms_ = limits.Integer("client_timeout_ms", 0, 3600000);
    max_bytes_ = limits.Integer("max_bytes", 0, INT64_MAX);
    max_tasks_ = limits.Integer("max_tasks", 1, 1024);
    max_log_ = limits.Integer("max_log_bytes", 1, INT64_MAX);
    deadline_ = Clock::now() + std::chrono::milliseconds(timeout_ms_);
    identity_ = Identity();
    if (request_.Has("architecture"))
      Require(
          identity_.Text("architecture") == request_.Text("architecture"),
          "Allocated architecture differs from admission");
    if (request_.Has("nodes")) {
      bool found = false;
      for (const auto& node : request_.Get("nodes").Items())
        found |= node.Text() == identity_.Text("node");
      Require(found, "Allocated node is not in the admitted runtime pool");
    }
    runtime_ = request_.Get("files");
    Require(runtime_.Keys().size() <= 4096, "Too many pinned runtime files");
    if (request_.Has("read_files")) {
      for (const auto& value : request_.Get("read_files").Items()) {
        const auto path = value.Text();
        Require(
            fs::path(path).is_absolute() && fs::path(path) == fs::path(path).lexically_normal(),
            "Read grants need canonical absolute file paths");
        read_files_.push_back(path);
      }
      Require(read_files_.size() <= 4096, "Too many runtime read grants");
    }
  }
  int Run() {
    Fd lock(::open((root_ / "worker.lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
    Require(::flock(lock.value, LOCK_EX | LOCK_NB) == 0, "This worker session already has an owner");
    Require(
        !fs::exists(root_ / "state.json") && !fs::exists(root_ / "result.json"), "A worker session cannot be replayed");
    for (const char* name : {"commands", "replies", "tasks"})
      PrivateDirectory(root_ / name, true);
    Check(process_guard::Enable());
    std::string error;
    bool stopped = false;
    try {
      State("starting", false, "");
      for (const auto& path : runtime_.Keys()) {
        Require(fs::path(path).is_absolute(), "Runtime files require absolute paths");
        Require(
            File(path, 8LL * 1024 * 1024 * 1024, [&] { CheckDeadline(); }).Text("sha256") ==
                runtime_.Text(path.c_str()),
            "Pinned runtime file changed: " + path);
      }
      if (request_.Has("tools")) {
        const auto tools = request_.Get("tools").Items();
        Require(tools.size() <= 64, "Too many compute tool aliases");
        for (const auto& tool : tools) {
          const auto name = tool.Text();
          Require(std::regex_match(name, std::regex("[A-Za-z0-9_+.-]{1,128}")), "Invalid compute tool alias");
          const char* search = ::getenv("PATH");
          Require(search != nullptr, "Compute tool search path is unavailable");
          std::istringstream paths(search);
          std::string item, resolved;
          while (std::getline(paths, item, ':')) {
            if (!fs::path(item).is_absolute())
              continue;
            const auto candidate = fs::path(item) / name;
            if (::access(candidate.c_str(), X_OK) == 0 && fs::is_regular_file(candidate)) {
              resolved = fs::canonical(candidate).string();
              break;
            }
          }
          Require(!resolved.empty(), "Compute tool is unavailable: " + name);
          const auto digest = File(resolved, 8LL * 1024 * 1024 * 1024, [&] { CheckDeadline(); }).Text("sha256");
          if (runtime_.Has(resolved.c_str()))
            Require(runtime_.Text(resolved.c_str()) == digest, "Qualified compute tool changed: " + name);
          runtime_.Set(resolved.c_str(), digest);
          tools_.Set(name.c_str(), resolved);
        }
      }
      if (request_.Has("tcp_port"))
        ReservePort(request_.Get("tcp_port"));
      State("ready", false, "");
      auto heartbeat = Clock::now();
      auto accounting = Clock::now();
      last_client_ = Clock::now();
      lease_started_ = true;
      while (!finished_) {
        CheckDeadline();
        Require(!fs::exists(root_ / "cancel"), "cancelled");
        Poll();
        std::ostringstream number;
        number << std::setfill('0') << std::setw(20) << next_ << ".json";
        const auto command_path = root_ / "commands" / number.str();
        if (fs::exists(command_path)) {
          const auto command = Json::Parse(Read(command_path));
          Require(
              command.Text("schema") == kSchema && command.Text("token") == token_ &&
                  command.Integer("sequence", 1, INT64_MAX) == next_,
              "Runtime command binding changed");
          Json response;
          response.Set("schema", kSchema);
          response.Set("id", id_);
          response.Set("token", token_);
          response.Set("sequence", next_);
          try {
            response.Set("result", Dispatch(command));
            response.Set("ok", true);
          } catch (const std::exception& failure) {
            response.Set("ok", false);
            response.Set("error", std::string(failure.what()).substr(0, 4096));
          }
          Atomic(root_ / "replies" / number.str(), response);
          Require(::unlink(command_path.c_str()) == 0, "Cannot retire processed command");
          ++next_;
        }
        if (Clock::now() >= heartbeat) {
          State("ready", false, "");
          heartbeat = Clock::now() + std::chrono::seconds(1);
        }
        if (Clock::now() >= accounting) {
          if (max_bytes_) {
            int64_t bytes = 0;
            Account(Fd(::open(workspace_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)), bytes);
          }
          accounting = Clock::now() + std::chrono::seconds(1);
        }
        ::poll(nullptr, 0, static_cast<int>(poll_ms_));
      }
      stopped = Retire();
      Require(stopped, "Runtime descendant retirement is unproved");
    } catch (const std::exception& failure) {
      error = failure.what();
      try {
        stopped = Retire();
      } catch (const std::exception& cleanup) {
        error += "; cleanup: " + std::string(cleanup.what());
      }
    }
    State("finished", stopped, error);
    Json result = Bound();
    result.Set("stopped", stopped);
    result.Set("error", error);
    result.Set("ok", stopped && error.empty());
    Json tasks;
    for (const auto& entry : tasks_)
      tasks.Set(entry.first.c_str(), TaskState(entry.second));
    result.Set("tasks", tasks);
    Atomic(root_ / "result.json", result);
    return stopped && error.empty() ? 0 : 1;
  }

 private:
  Json Bound() const {
    Json value;
    value.Set("schema", kSchema);
    value.Set("id", id_);
    value.Set("token", token_);
    value.Set("identity", identity_);
    return value;
  }
  void CheckDeadline() {
    Require(!interrupted, "cancelled by signal");
    Require(!fs::exists(root_ / "cancel"), "cancelled");
    Require(Clock::now() < deadline_, "Runtime session deadline exceeded");
    if (lease_started_ && client_ms_) {
      if (fs::exists(root_ / "client.json")) {
        // Atomic lease replacement may unlink the opened version. Its stable
        // bytes remain a valid snapshot; token and sequence still fence it.
        const auto client = Json::Parse(Read(root_ / "client.json", 4096, true));
        Require(client.Text("token") == token_, "Controller lease binding changed");
        const auto sequence = client.Integer("sequence", 0, INT64_MAX);
        if (sequence > client_sequence_) {
          client_sequence_ = sequence;
          last_client_ = Clock::now();
        }
      }
      Require(Clock::now() - last_client_ < std::chrono::milliseconds(client_ms_), "Controller lease expired");
    }
    // Hashing, HTTP and accounting must not suspend per-task supervision.
    Poll();
  }
  void Account(Fd directory, int64_t& bytes) {
    DIR* opened = ::fdopendir(directory.value);
    Require(opened != nullptr, "Cannot inspect workspace directory");
    directory.value = -1;
    const auto close_directory = [](DIR* value) { ::closedir(value); };
    std::unique_ptr<DIR, decltype(close_directory)> stream(opened, close_directory);
    for (;;) {
      CheckDeadline();
      errno = 0;
      const auto* entry = ::readdir(stream.get());
      if (!entry) {
        Require(errno == 0, "Cannot enumerate workspace directory");
        return;
      }
      const std::string name(entry->d_name);
      if (name == "." || name == "..")
        continue;
      struct stat info {};
      if (::fstatat(::dirfd(stream.get()), name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT)
          continue; // Native finalization and preview cleanup remove entries.
        throw std::runtime_error("Cannot inspect workspace entry");
      }
      if (S_ISREG(info.st_mode)) {
        Require(info.st_size >= 0 && info.st_size <= max_bytes_ - bytes, "Workspace storage allowance exceeded");
        bytes += info.st_size;
      } else if (S_ISDIR(info.st_mode)) {
        const int child =
            ::openat(::dirfd(stream.get()), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0 && errno == ENOENT)
          continue;
        Account(Fd(child), bytes);
      } else {
        Require(S_ISLNK(info.st_mode), "Invalid workspace file type");
      }
    }
  }
  fs::path Confined(const std::string& value, bool exists = true) const {
    fs::path path(value);
    if (path.is_relative())
      path = workspace_ / path;
    Require(path == path.lexically_normal(), "Noncanonical workspace path");
    auto relative = path.lexically_relative(workspace_);
    Require(!relative.empty() && *relative.begin() != "..", "Path escapes the runtime workspace");
    Require((exists ? fs::canonical(path) : fs::weakly_canonical(path)) == path, "Workspace path traverses a symlink");
    return path;
  }
  void State(const char* status, bool stopped, const std::string& error) {
    auto value = Bound();
    value.Set("status", status);
    value.Set("stopped", stopped);
    value.Set("error", error);
    value.Set("time_ms", WallTimeMs());
    value.Set("started_ms", started_ms_);
    value.Set("sequence", next_ - 1);
    Json tasks;
    for (const auto& entry : tasks_)
      tasks.Set(entry.first.c_str(), TaskState(entry.second));
    value.Set("tasks", tasks);
    value.Set("tools", tools_);
    if (port_)
      value.Set("port", static_cast<int64_t>(port_));
    Atomic(root_ / "state.json", value);
  }
  Json TaskState(const Task& task) const {
    Json result;
    result.Set("id", task.id);
    result.Set("pid", static_cast<int64_t>(task.pid));
    result.Set("stopped", task.stopped);
    result.Set("timed_out", task.timed_out);
    if (task.code)
      result.Set("returncode", static_cast<int64_t>(*task.code));
    else
      result.Null("returncode");
    return result;
  }
  Task& GetTask(const Json& command) {
    auto found = tasks_.find(command.Text("task"));
    Require(found != tasks_.end(), "Unknown runtime task");
    return found->second;
  }
  void Poll(bool retiring = false) {
    for (auto& entry : tasks_) {
      auto& task = entry.second;
      if (!task.code) {
        int status = 0;
        const auto found = ::waitpid(task.pid, &status, WNOHANG);
        Require(found >= 0 || errno == EINTR, "Lost native task exit status");
        if (found > 0) {
          task.code = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
          const auto proof = process_guard::ReadReceipt(task.proof.value, task.receipt, task.header);
          Require(proof.ok() && *proof == 'C', "Native task lost its exact descendant-retirement receipt");
          task.stopped = true;
          task.pipe.Close();
        } else if (!retiring && Clock::now() >= task.deadline) {
          task.timed_out = true;
          Check(process_guard::RequestSignal(task.proof.value, task.receipt, task.header, SIGTERM));
        }
      }
      if (task.pipe.value >= 0) {
        if (!task.input.empty()) {
          const auto count = ::write(task.pipe.value, task.input.data(), task.input.size());
          if (count > 0)
            task.input.erase(0, count);
          else if (count < 0 && errno == EPIPE) {
            task.pipe.Close();
            task.input.clear();
          } else
            Require(count < 0 && (errno == EAGAIN || errno == EINTR), "Native command pipe failed");
        }
        if (task.close_input && task.input.empty())
          task.pipe.Close();
      }
      if (!retiring) {
        for (const auto& path : {task.output, task.errors}) {
          struct stat info {};
          Require(::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode), "Native log was replaced");
          Require(info.st_size <= max_log_, "Native log allowance exceeded");
        }
      }
    }
  }
  bool Retire() {
    bool damaged = false;
    for (auto& entry : tasks_) {
      if (entry.second.code && !entry.second.stopped)
        damaged = true;
      if (!entry.second.code) {
        auto& task = entry.second;
        if (!process_guard::RequestSignal(task.proof.value, task.receipt, task.header, SIGTERM).ok())
          damaged = true;
      }
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(2 * grace_ms_ + 2 * poll_ms_);
    while (!damaged && Clock::now() < deadline) {
      try {
        Poll(true);
      } catch (...) {
        damaged = true;
        break;
      }
      bool done = true;
      for (const auto& entry : tasks_)
        done &= entry.second.stopped;
      if (done) {
        const auto proof = process_guard::Reap();
        Check(proof.status());
        if (*proof)
          return true;
      }
      ::poll(nullptr, 0, static_cast<int>(poll_ms_));
    }
    // Even a damaged per-task receipt cannot skip outer containment. This path
    // never authorizes artifact adoption: the session records a cleanup error.
    const auto retired = process_guard::Retire(grace_ms_, poll_ms_);
    Require(
        !damaged,
        "Native task retirement evidence was lost; outer cleanup " +
            std::string(retired.ok() ? "completed" : "is unproved"));
    return retired.ok();
  }
  Json Launch(const Json& command) {
    Require(
        tasks_.size() < static_cast<size_t>(max_tasks_),
        "Runtime task history is full; retire and forget completed tasks");
    auto spec = command.Get("spec");
    Task task;
    task.id = spec.Text("id");
    task.nonce = Nonce();
    Require(
        std::regex_match(task.id, std::regex("[a-f0-9]{32}")) && !tasks_.count(task.id),
        "Invalid or reused native task id");
    std::vector<std::string> argv;
    for (const auto& value : spec.Get("argv").Items()) {
      auto arg = value.Text();
      Require(arg.size() <= 65536, "Native argument too large");
      argv.push_back(arg);
    }
    if (!argv.empty() && tools_.Has(argv[0].c_str()))
      argv[0] = tools_.Text(argv[0].c_str());
    Require(
        !argv.empty() && argv.size() <= 4096 && fs::path(argv[0]).is_absolute(),
        "Native command requires bounded argv and absolute executable");
    Require(runtime_.Has(argv[0].c_str()), "Native executable was not pinned at admission");
    Fd executable_fd;
    const auto executable = File(argv[0], 8LL * 1024 * 1024 * 1024, [&] { CheckDeadline(); }, &executable_fd);
    Require(executable.Text("sha256") == runtime_.Text(argv[0].c_str()), "Native executable changed before launch");
    auto cwd = Confined(spec.Text("cwd"));
    auto output = Confined(spec.Text("stdout"), false);
    auto errors = Confined(spec.Text("stderr"), false);
    task.output = output;
    task.errors = errors;
    Require(fs::is_directory(cwd), "Native working directory is unavailable");
    auto environment = spec.Get("environment");
    Require(environment.Keys().size() <= 256, "Native environment exceeds its bound");
    for (const auto& key : environment.Keys())
      Require(
          std::regex_match(key, std::regex("[A-Za-z_][A-Za-z0-9_]*")) && environment.Text(key.c_str()).size() <= 65536,
          "Invalid native environment");
    task.deadline = Clock::now() + std::chrono::milliseconds(spec.Integer("timeout_ms", 1, timeout_ms_));
    const auto directory = root_ / "tasks" / task.id;
    Require(::mkdir(directory.c_str(), 0700) == 0, "Task identity already exists");
    task.receipt = directory / "retirement";
    task.header = process_guard::ReceiptHeader(token_, task.nonce);
    task.proof = Fd(::open(task.receipt.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    WriteAll(task.proof.value, task.header + "0P");
    Require(::fsync(task.proof.value) == 0, "Cannot persist task permit");
    int pipes[2];
    Require(::pipe2(pipes, O_CLOEXEC) == 0, "Cannot create native command pipe");
    Fd input(pipes[0]);
    task.pipe = Fd(pipes[1]);
    Fd out(::open(output.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600));
    Fd err(::open(errors.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600));
    for (const int descriptor : {out.value, err.value}) {
      struct stat info {};
      Require(::fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode), "Native logs require regular files");
    }
    const auto parent = ::getpid();
    task.pid = ::fork();
    Require(task.pid >= 0, "Cannot launch native task owner");
    if (!task.pid) {
      try {
        ::signal(SIGPIPE, SIG_DFL);
        Require(
            ::dup2(input.value, STDIN_FILENO) >= 0 && ::dup2(out.value, STDOUT_FILENO) >= 0 &&
                ::dup2(err.value, STDERR_FILENO) >= 0,
            "Cannot install native task descriptors");
        task.pipe.Close();
        task.proof.Close();
        input.Close();
        out.Close();
        err.Close();
        // The guard does not exec. CLOEXEC alone would retain the other task's
        // pipe ends and the worker lock for the entire guard lifetime.
        DIR* descriptors = ::opendir("/proc/self/fd");
        Require(descriptors != nullptr, "Cannot enumerate inherited descriptors");
        while (const auto* entry = ::readdir(descriptors)) {
          char* end = nullptr;
          const long descriptor = std::strtol(entry->d_name, &end, 10);
          if (end != entry->d_name && !*end && descriptor > 2 && descriptor != ::dirfd(descriptors) &&
              descriptor != executable_fd.value)
            ::close(static_cast<int>(descriptor));
        }
        ::closedir(descriptors);
        Require(::chdir(cwd.c_str()) == 0, "Cannot enter native workspace");
        for (const auto& key : environment.Keys())
          Require(
              ::setenv(key.c_str(), environment.Text(key.c_str()).c_str(), 1) == 0, "Cannot set native environment");
        ::setenv("HSTREAM_UI_PARENT_PID", std::to_string(parent).c_str(), 1);
        if (max_bytes_) {
          struct rlimit limit {
            static_cast<rlim_t>(max_bytes_), static_cast<rlim_t>(max_bytes_)
          };
          Require(::setrlimit(RLIMIT_FSIZE, &limit) == 0, "Cannot set native file allowance");
        }
        const auto result = process_guard::Run(
            task.receipt, token_, task.nonce, parent, grace_ms_, poll_ms_, argv, executable_fd.value);
        if (!result.ok()) {
          std::cerr << result.status() << '\n';
          (void)process_guard::Retire(grace_ms_, poll_ms_);
          ::_exit(125);
        }
        ::_exit(*result);
      } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        ::_exit(125);
      }
    }
    Require(
        ::fcntl(task.pipe.value, F_SETFL, ::fcntl(task.pipe.value, F_GETFL) | O_NONBLOCK) == 0,
        "Cannot bound native command writes");
    auto inserted = tasks_.emplace(task.id, std::move(task));
    return TaskState(inserted.first->second);
  }
  void ReservePort(const Json& value) {
    const auto minimum = value.Integer("minimum", 1024, 65535);
    const auto maximum = value.Integer("maximum", minimum + 1, 65536);
    Require(maximum - minimum <= 1000, "Port lease range exceeds its bound");
    auto root = fs::path(value.Text("lease_root"));
    Require(root.is_absolute() && root == fs::weakly_canonical(root), "Invalid port lease root");
    PrivateDirectory(root, true);
    root /= Host();
    PrivateDirectory(root, true);
    for (int port = minimum; port < maximum; ++port) {
      Fd lease(::open((root / std::to_string(port)).c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
      if (::flock(lease.value, LOCK_EX | LOCK_NB) != 0)
        continue;
      Fd socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_port = htons(port);
      address.sin_addr.s_addr = INADDR_ANY;
      if (::bind(socket.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        continue;
      port_ = port;
      port_lease_ = std::move(lease);
      return;
    }
    throw std::runtime_error("All admitted TCP listener slots are occupied");
  }
  Json Http(const Json& command) {
    const int port = command.Integer("port", 1024, 65535);
    const auto path = command.Text("path");
    Require(
        !path.empty() && path.front() == '/' && path.size() <= 8192 && path.find_first_of("\r\n") == std::string::npos,
        "Invalid loopback request path");
    const auto method = command.Text("method");
    Require(method == "GET" || method == "POST", "Unsupported loopback request method");
    const auto body = command.Has("body") ? command.Text("body") : "";
    Require(body.size() <= kPipeLimit, "Loopback request exceeds its bound");
    struct Output {
      std::string data;
      bool overflow = false;
    } output;
    auto* curl = curl_easy_init();
    Require(curl != nullptr, "Cannot initialize bounded loopback request");
    const auto url = "http://127.0.0.1:" + std::to_string(port) + path;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Origin: http://127.0.0.1:" + std::to_string(port)).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(
        curl, CURLOPT_XFERINFOFUNCTION, +[](void* pointer, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
          try {
            static_cast<Worker*>(pointer)->CheckDeadline();
            return 0;
          } catch (...) {
            return 1;
          }
        });
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(command.Integer("timeout_ms", 1, 10000)));
    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION, +[](char* data, size_t size, size_t count, void* pointer) -> size_t {
          auto& target = *static_cast<Output*>(pointer);
          const auto bytes = size * count;
          if (bytes > 1024 * 1024 - target.data.size()) {
            target.overflow = true;
            return 0;
          }
          target.data.append(data, bytes);
          return bytes;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);
    if (method == "POST") {
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    auto* multi = curl_multi_init();
    CURLcode result = CURLE_FAILED_INIT;
    std::string failure;
    try {
      Require(multi != nullptr && curl_multi_add_handle(multi, curl) == CURLM_OK, "Cannot start loopback request");
      int running = 0;
      do {
        CheckDeadline();
        Require(curl_multi_perform(multi, &running) == CURLM_OK, "Cannot advance loopback request");
        if (running)
          Require(
              curl_multi_poll(multi, nullptr, 0, static_cast<int>(poll_ms_), nullptr) == CURLM_OK,
              "Cannot wait for loopback request");
      } while (running);
      int remaining = 0;
      while (auto* message = curl_multi_info_read(multi, &remaining)) {
        if (message->msg == CURLMSG_DONE)
          result = message->data.result;
      }
    } catch (const std::exception& error) {
      failure = error.what();
    }
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (multi) {
      curl_multi_remove_handle(multi, curl);
      curl_multi_cleanup(multi);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    Require(failure.empty(), failure);
    Require(result == CURLE_OK && !output.overflow, "Bounded loopback request failed");
    Json response;
    response.Set("status", static_cast<int64_t>(status));
    response.Set("body", Base64(output.data));
    return response;
  }
  Json Dispatch(const Json& command) {
    const auto operation = command.Text("operation");
    if (operation == "launch")
      return Launch(command);
    if (operation == "inspect" || operation == "read") {
      const auto name = command.Text("path");
      const auto path =
          runtime_.Has(name.c_str()) || std::find(read_files_.begin(), read_files_.end(), name) != read_files_.end()
          ? fs::path(name)
          : Confined(name);
      std::string contents;
      auto actual = File(
          path,
          command.Integer("maximum", 0, operation == "read" ? 1024 * 1024 : INT64_MAX),
          [&] { CheckDeadline(); },
          nullptr,
          operation == "read" ? &contents : nullptr);
      if (command.Has("expected")) {
        const auto expected = command.Get("expected");
        for (const auto& key : expected.Keys())
          Require(
              actual.Get(key.c_str()).Dump() == expected.Get(key.c_str()).Dump(),
              "Compute artifact identity changed: " + key);
      }
      if (operation == "read")
        actual.Set("data", Base64(contents));
      return actual;
    }
    if (operation == "http")
      return Http(command);
    if (operation == "retire" || operation == "finish") {
      const auto stopped = Retire();
      Require(stopped, "Runtime descendants did not retire");
      if (operation == "finish")
        finished_ = true;
      Json result;
      result.Set("stopped", true);
      return result;
    }
    auto& task = GetTask(command);
    if (operation == "poll") {
      Poll();
      return TaskState(task);
    }
    if (operation == "retire-task") {
      Check(process_guard::RequestSignal(task.proof.value, task.receipt, task.header, SIGTERM));
      const auto deadline = Clock::now() + std::chrono::milliseconds(2 * grace_ms_ + 2 * poll_ms_);
      while (!task.code && Clock::now() < deadline) {
        Poll();
        ::poll(nullptr, 0, static_cast<int>(poll_ms_));
      }
      Require(task.stopped, "Native task descendant retirement is unproved");
      return TaskState(task);
    }
    if (operation == "stdin") {
      Require(task.pipe.value >= 0 && !task.close_input && !task.code, "Native input has closed");
      auto data = Unbase64(command.Text("data"));
      const auto count = std::min(data.size(), kPipeLimit - task.input.size());
      task.input.append(data, 0, count);
      Json result;
      result.Set("accepted", static_cast<int64_t>(count));
      return result;
    }
    if (operation == "stdin-close") {
      task.close_input = true;
      return TaskState(task);
    }
    if (operation == "signal") {
      const int number = command.Integer("signal", 1, 64);
      Check(process_guard::RequestSignal(task.proof.value, task.receipt, task.header, number));
      return TaskState(task);
    }
    if (operation == "forget") {
      Require(task.stopped && task.code.has_value(), "Cannot forget an owned task");
      const auto id = task.id;
      tasks_.erase(id);
      Json result;
      result.Set("forgotten", true);
      return result;
    }
    throw std::runtime_error("Unsupported runtime operation");
  }
  fs::path root_, workspace_;
  Json request_, runtime_, identity_, tools_;
  std::string id_, token_;
  int64_t poll_ms_, grace_ms_, timeout_ms_, client_ms_, max_bytes_, max_log_, max_tasks_, next_ = 1;
  int64_t started_ms_ = WallTimeMs();
  Clock::time_point deadline_, last_client_;
  int64_t client_sequence_ = -1;
  bool lease_started_ = false;
  std::map<std::string, Task> tasks_;
  std::vector<std::string> read_files_;
  Fd port_lease_;
  int port_ = 0;
  bool finished_ = false;
};
} // namespace

Json Capabilities() {
  Json result;
  result.Set("schema", kSchema);
  result.Set("protocol", static_cast<int64_t>(1));
  auto operations = Json::Array();
  for (const char* operation :
       {"launch",
        "stdin",
        "stdin-close",
        "signal",
        "poll",
        "retire-task",
        "retire",
        "forget",
        "inspect",
        "read",
        "http",
        "finish"})
    operations.Append(Json::String(operation));
  result.Set("operations", operations);
  result.Set("retirement", "pidfd-echild-v1");
  return result;
}
int Run(const fs::path& request) {
  ::umask(0077);
  struct sigaction action {};
  action.sa_handler = Stop;
  ::sigemptyset(&action.sa_mask);
  for (int number : {SIGTERM, SIGINT, SIGHUP})
    Require(::sigaction(number, &action, nullptr) == 0, "Cannot install runtime cancellation");
  ::signal(SIGPIPE, SIG_IGN);
  Worker worker(request);
  return worker.Run();
}
} // namespace hm::runtime_worker
