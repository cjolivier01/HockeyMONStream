#include "hstream/src/libs/common/ProcessGuard.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>

namespace hm::process_guard {
namespace {
pid_t scope_pid = 0;
volatile sig_atomic_t interrupted = 0;
void stop(int number) {
  interrupted = number;
}
absl::Status error(const char* message) {
  return absl::InternalError(std::string(message) + ": " + std::strerror(errno));
}
struct Descriptor {
  int value;
  ~Descriptor() {
    if (value >= 0)
      ::close(value);
  }
};
absl::Status write_byte(int descriptor, off_t offset, char value) {
  ssize_t count;
  do {
    count = ::pwrite(descriptor, &value, 1, offset);
  } while (count < 0 && errno == EINTR);
  if (count != 1 || ::fsync(descriptor) != 0)
    return error("Cannot persist process guard state");
  return absl::OkStatus();
}
absl::StatusOr<std::pair<pid_t, uint64_t>> process(pid_t pid) {
  std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  if (!std::getline(input, line))
    return absl::NotFoundError("Process identity disappeared");
  const auto end = line.rfind(')');
  if (end == std::string::npos)
    return absl::UnavailableError("Invalid process identity");
  std::istringstream fields(line.substr(end + 2));
  std::string value;
  pid_t parent = 0;
  uint64_t start = 0;
  for (int index = 3; index <= 22; ++index) {
    if (!(fields >> value))
      return absl::UnavailableError("Incomplete process identity");
    if (index == 4)
      parent = std::stol(value);
    if (index == 22)
      start = std::stoull(value);
  }
  return std::pair<pid_t, uint64_t>{parent, start};
}
} // namespace

bool Enabled() {
  int enabled = 0;
  return scope_pid == ::getpid() && ::prctl(PR_GET_CHILD_SUBREAPER, &enabled) == 0 && enabled;
}

absl::Status Enable() {
  if (Enabled())
    return absl::OkStatus();
  siginfo_t status{};
  if (::waitid(P_ALL, 0, &status, WEXITED | WNOHANG | WNOWAIT) == 0 || errno != ECHILD)
    return absl::FailedPreconditionError("Child proof must be established before any command is launched");
  if (::prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)
    return error("Cannot establish process descendant ownership");
  struct sigaction child_status{};
  child_status.sa_handler = SIG_DFL;
  ::sigemptyset(&child_status.sa_mask);
  if (::sigaction(SIGCHLD, &child_status, nullptr) != 0)
    return error("Cannot retain process descendant exit statuses");
  scope_pid = ::getpid();
  return absl::OkStatus();
}

absl::StatusOr<bool> Reap() {
  if (!Enabled())
    return absl::FailedPreconditionError("No isolated process descendant scope is active");
  for (;;) {
    const pid_t child = ::waitpid(-1, nullptr, WNOHANG);
    if (child > 0)
      continue;
    if (child == 0)
      return false;
    if (errno == EINTR)
      continue;
    if (errno == ECHILD)
      return true;
    return error("Cannot prove process descendants stopped");
  }
}

absl::Status Signal(int number) {
  if (!Enabled())
    return absl::FailedPreconditionError("No isolated process descendant scope is active");
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
  try {
    // Only direct children need signaling. Escaped grandchildren become direct
    // children when their parent exits, and the next pass discovers them. The
    // kernel ECHILD check, never this enumeration, proves final shutdown.
    for (const auto& entry : std::filesystem::directory_iterator("/proc")) {
      const auto name = entry.path().filename().string();
      if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
        continue;
      const pid_t pid = std::stol(name);
      const auto before = process(pid);
      if (!before.ok() || before->first != ::getpid())
        continue;
      Descriptor pinned{static_cast<int>(::syscall(SYS_pidfd_open, pid, 0))};
      if (pinned.value < 0) {
        if (errno == ESRCH)
          continue;
        return error("Cannot pin process descendant");
      }
      const auto current = process(pid);
      if (!current.ok() || *current != *before)
        continue;
      if (::syscall(SYS_pidfd_send_signal, pinned.value, number, nullptr, 0) != 0 && errno != ESRCH)
        return error("Cannot signal process descendant");
    }
    return absl::OkStatus();
  } catch (const std::exception& exception) {
    return absl::UnavailableError("Cannot inspect process descendants: " + std::string(exception.what()));
  }
#else
  return absl::UnimplementedError("Process descendant signaling requires pidfd support");
#endif
}

absl::Status Retire(int64_t grace_ms, int64_t poll_ms) {
  if (grace_ms <= 0 || poll_ms <= 0 || poll_ms > grace_ms || grace_ms > 24 * 60 * 60 * 1000LL)
    return absl::InvalidArgumentError("Invalid explicit process cleanup timing");
  const auto started = std::chrono::steady_clock::now();
  for (;;) {
    const auto stopped = Reap();
    if (!stopped.ok())
      return stopped.status();
    if (*stopped)
      return absl::OkStatus();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (elapsed >= 2 * grace_ms)
      return absl::DeadlineExceededError("Cannot prove isolated descendants stopped; game stays reserved");
    const auto signalled = Signal(elapsed >= grace_ms ? SIGKILL : SIGTERM);
    if (!signalled.ok())
      return signalled;
    ::poll(nullptr, 0, static_cast<int>(poll_ms));
  }
}

std::string ReceiptHeader(const std::string& token, const std::string& nonce) {
  return "hstream-process-guard-v1\n" + token + "\n" + nonce + "\n";
}

absl::StatusOr<char> ReadReceipt(int descriptor, const std::filesystem::path& path, const std::string& header) {
  struct stat pinned{}, named{};
  if (::fstat(descriptor, &pinned) != 0 || ::lstat(path.c_str(), &named) != 0 || !S_ISREG(pinned.st_mode) ||
      !S_ISREG(named.st_mode) || pinned.st_uid != ::geteuid() || (pinned.st_mode & 0777) != 0600 ||
      pinned.st_dev != named.st_dev || pinned.st_ino != named.st_ino ||
      pinned.st_size != static_cast<off_t>(header.size() + 2))
    return absl::FailedPreconditionError("Process guard receipt identity changed; cleanup needs recovery");
  std::string contents(header.size() + 2, '\0');
  ssize_t count;
  do {
    count = ::pread(descriptor, contents.data(), contents.size(), 0);
  } while (count < 0 && errno == EINTR);
  if (count != static_cast<ssize_t>(contents.size()) || contents.compare(0, header.size(), header) != 0 ||
      std::string("0TIK").find(contents[header.size()]) == std::string::npos ||
      std::string("PRCF").find(contents.back()) == std::string::npos)
    return absl::FailedPreconditionError("Invalid process guard receipt; cleanup needs recovery");
  return contents.back();
}

absl::Status RequestSignal(int descriptor, const std::filesystem::path& path, const std::string& header, int number) {
  const auto state = ReadReceipt(descriptor, path, header);
  if (!state.ok())
    return state.status();
  if (*state == 'C')
    return absl::OkStatus();
  if (*state == 'F')
    return absl::FailedPreconditionError("Process guard could not prove cleanup");
  const char request = number == SIGKILL ? 'K' : number == SIGINT ? 'I' : number == SIGTERM ? 'T' : '\0';
  if (!request)
    return absl::InvalidArgumentError("Unsupported process guard signal");
  char previous = 0;
  if (::pread(descriptor, &previous, 1, header.size()) != 1)
    return error("Cannot inspect process guard request");
  if (previous == 'K')
    return absl::OkStatus();
  return write_byte(descriptor, header.size(), request);
}

absl::StatusOr<int> Run(
    const std::filesystem::path& receipt,
    const std::string& token,
    const std::string& nonce,
    int64_t parent_pid,
    int64_t grace_ms,
    int64_t poll_ms,
    const std::vector<std::string>& command) {
  if (command.empty() || command.front().empty() || grace_ms <= 0 || poll_ms <= 0 || poll_ms > grace_ms ||
      grace_ms > 24 * 60 * 60 * 1000LL)
    return absl::InvalidArgumentError("Invalid process guard command or explicit cleanup timing");
  const auto enabled = Enable();
  if (!enabled.ok())
    return enabled;
  Descriptor record{::open(receipt.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC)};
  if (record.value < 0)
    return error("Cannot open process guard receipt");
  if (::flock(record.value, LOCK_EX | LOCK_NB) != 0)
    return absl::FailedPreconditionError("Process guard permit already has a supervisor");
  const auto header = ReceiptHeader(token, nonce);
  const auto initial = ReadReceipt(record.value, receipt, header);
  if (!initial.ok())
    return initial.status();
  if (*initial != 'P')
    return absl::FailedPreconditionError("Process guard permit was already consumed");
  const auto started = write_byte(record.value, header.size() + 1, 'R');
  if (!started.ok())
    return started;
  char initial_request = 0;
  if (::pread(record.value, &initial_request, 1, header.size()) != 1)
    return error("Cannot inspect process guard startup cancellation");
  if (initial_request != '0') {
    const auto completed = write_byte(record.value, header.size() + 1, 'C');
    if (!completed.ok())
      return completed;
    return 128 + (initial_request == 'K' ? SIGKILL : initial_request == 'I' ? SIGINT : SIGTERM);
  }
  // The proof descriptor is CLOEXEC and is explicitly closed in the worker.
  // Only this subreaper can acknowledge the kernel's final ECHILD result.
  struct sigaction action{};
  action.sa_handler = stop;
  ::sigemptyset(&action.sa_mask);
  for (const int number : {SIGINT, SIGTERM, SIGHUP}) {
    if (::sigaction(number, &action, nullptr) != 0)
      return error("Cannot install process guard cancellation");
  }
  const pid_t parent = ::getppid();
  if (parent_pid <= 1 || parent_pid != parent)
    return absl::FailedPreconditionError("Process guard has lost its exact launching parent");
  if (const char* expected = ::getenv("HSTREAM_UI_PARENT_PID")) {
    if (std::to_string(parent) != expected)
      return absl::FailedPreconditionError("Stale process guard parent");
  }
  if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || ::getppid() != parent)
    return absl::FailedPreconditionError("Process guard parent changed");
  const std::string own_pid = std::to_string(::getpid());
  if (::getenv("HSTREAM_UI_PARENT_PID"))
    ::setenv("HSTREAM_UI_PARENT_PID", own_pid.c_str(), 1);
  std::vector<char*> arguments;
  for (const auto& value : command)
    arguments.push_back(const_cast<char*>(value.c_str()));
  arguments.push_back(nullptr);
  const pid_t root = ::fork();
  if (root < 0) {
    (void)write_byte(record.value, header.size() + 1, 'C');
    return error("Cannot launch process guard worker");
  }
  if (!root) {
    ::close(record.value);
    for (const int number : {SIGINT, SIGTERM, SIGHUP})
      ::signal(number, SIG_DFL);
    ::execvp(arguments.front(), arguments.data());
    ::_exit(127);
  }
  int root_status = 0;
  bool root_finished = false;
  using Clock = std::chrono::steady_clock;
  std::optional<Clock::time_point> retirement;
  int requested = 0;
  for (;;) {
    bool no_children = false;
    for (;;) {
      int status = 0;
      const pid_t child = ::waitpid(-1, &status, WNOHANG);
      if (child > 0) {
        if (child == root) {
          root_status = status;
          root_finished = true;
        }
        continue;
      }
      if (child < 0 && errno == EINTR)
        continue;
      if (child < 0 && errno == ECHILD)
        no_children = true;
      else if (child < 0)
        return error("Cannot reap guarded process descendants");
      break;
    }
    if (no_children) {
      if (!root_finished)
        return absl::FailedPreconditionError("Process guard lost its worker status");
      const auto completed = write_byte(record.value, header.size() + 1, 'C');
      if (!completed.ok())
        return completed;
      return WIFEXITED(root_status) ? WEXITSTATUS(root_status) : 128 + WTERMSIG(root_status);
    }
    char control = 0;
    if (::pread(record.value, &control, 1, header.size()) != 1)
      return error("Cannot read process guard cancellation");
    if (control == 'K')
      requested = SIGKILL;
    else if (requested != SIGKILL && (control == 'I' || interrupted == SIGINT))
      requested = SIGINT;
    else if (requested != SIGKILL && (control == 'T' || interrupted))
      requested = SIGTERM;
    if ((root_finished || requested) && !retirement)
      retirement = Clock::now();
    if (retirement) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - *retirement).count();
      const int number = requested == SIGKILL || elapsed >= grace_ms ? SIGKILL : requested ? requested : SIGTERM;
      const auto signalled = Signal(number);
      if (!signalled.ok())
        return signalled;
      if (elapsed >= 2 * grace_ms) {
        (void)write_byte(record.value, header.size() + 1, 'F');
        return absl::DeadlineExceededError("Process guard could not prove descendant shutdown; game stays reserved");
      }
    }
    ::poll(nullptr, 0, static_cast<int>(poll_ms));
  }
}
} // namespace hm::process_guard
