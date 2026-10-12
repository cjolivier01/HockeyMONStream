#include "hstream/src/libs/common/ShutdownWatchdog.h"

#include "hstream/src/libs/common/ProcessDiagnostics.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <map>
#include <string>

namespace hm {
namespace {
constexpr int kShutdownWatchdogExitCode = 70;

// Reads a short /proc entry, trimming the trailing newline. Raw syscalls only:
// this runs while another thread owns a lock we cannot identify yet.
std::string read_proc_field(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return std::string();
  }
  char buffer[256];
  const ssize_t count = ::read(fd, buffer, sizeof(buffer) - 1);
  ::close(fd);
  if (count <= 0) {
    return std::string();
  }
  size_t length = static_cast<size_t>(count);
  while (length && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r')) {
    --length;
  }
  return std::string(buffer, length);
}

// Names the kernel routine every thread is parked in. Reading /proc is the
// only introspection left once the watchdog decides to hard-exit: asking
// GStreamer anything means taking GStreamer locks, and an unidentified
// GStreamer lock is the whole reason we are here.
//
// Identical entries collapse to one counted line. CUDA and TensorRT keep well
// over a hundred idle workers parked on the same futex, and listing them
// individually buries the one thread that is parked somewhere interesting.
std::string describe_threads(long blocked_tid) {
  DIR* tasks = ::opendir("/proc/self/task");
  if (!tasks) {
    return std::string();
  }
  std::string blocked;
  std::map<std::string, int> idle;
  while (const dirent* entry = ::readdir(tasks)) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    const std::string task = std::string("/proc/self/task/") + entry->d_name;
    std::string name = read_proc_field(task + "/comm");
    // CUDA and TensorRT name pool threads "pool#index". Fold the index away so
    // seventy-odd identical siblings collapse instead of one line each.
    const size_t index = name.rfind('#');
    if (index != std::string::npos && index + 1 < name.size() &&
        name.find_first_not_of("0123456789", index + 1) == std::string::npos) {
      name.replace(index + 1, std::string::npos, "*");
    }
    const std::string state = "name=" + name + " wchan=" + read_proc_field(task + "/wchan");
    if (g_ascii_strtoll(entry->d_name, nullptr, 10) == blocked_tid) {
      blocked = "  tid=" + std::string(entry->d_name) + ' ' + state + "  <-- stalled transition\n";
    } else {
      ++idle[state];
    }
  }
  ::closedir(tasks);
  std::string report = blocked;
  for (const auto& entry : idle) {
    report += "  " + entry.first + " threads=" + std::to_string(entry.second) + "\n";
  }
  return report;
}

} // namespace

ShutdownWatchdog::ShutdownWatchdog(const char* operation, gint64 budget_us)
    : operation_(operation ? operation : "shutdown"), blocked_tid_(::syscall(SYS_gettid)) {
  if (budget_us <= 0) {
    return;
  }
  try {
    worker_ = std::thread([this, budget_us]() {
      std::unique_lock<std::mutex> lock(mutex_);
      const bool completed = finished_.wait_for(lock, std::chrono::microseconds(budget_us), [this]() { return done_; });
      lock.unlock();
      // Drop the lock before firing. fire() writes to stderr, which may be a
      // pipe to a parent that is not draining it, and the destructor blocks
      // on this same mutex -- holding it across the write turns the hang we
      // are reporting into a second, quieter one. Releasing it lets a late
      // completion proceed instead, which is harmless: the process is going
      // down either way, and _exit() cannot be half-done.
      if (!completed) {
        fire(operation_, blocked_tid_, budget_us);
      }
    });
  } catch (...) {
    // std::thread's constructor throws system_error when the process is out
    // of threads and bad_alloc when it is out of memory -- both plausible
    // during a teardown that follows an allocation failure. An unguarded
    // transition is still better than unwinding into a caller that would
    // leak app_lock or trip a noexcept destructor.
  }
}

ShutdownWatchdog::~ShutdownWatchdog() {
  if (!worker_.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = true;
  }
  finished_.notify_all();
  worker_.join();
}

void ShutdownWatchdog::fire(const char* operation, long blocked_tid, gint64 budget_us) {
  // Self-guard, same reasoning as crash_timeout() in ProcessDiagnostics.cpp:
  // every step below can itself block on the resource that wedged us. The
  // default SIGALRM action terminates the process, so this needs no handler.
  std::signal(SIGALRM, SIG_DFL);
  ::alarm(5);
  const std::string threads = describe_threads(blocked_tid);
  // stderr and the diagnostics writer only, never GLib or GStreamer logging:
  // the whole premise is that some GStreamer lock is unavailable.
  std::fflush(stdout);
  std::fprintf(
      stderr,
      "FATAL: %s did not finish within %" G_GINT64_FORMAT " s; shutdown is wedged. Thread states:\n%s",
      operation,
      budget_us / G_USEC_PER_SEC,
      threads.c_str());
  std::fflush(stderr);
  hm::diagnostics::Breadcrumb("shutdown-watchdog", operation);
  hm::diagnostics::Log("shutdown-watchdog", std::string(operation) + " wedged; thread states:\n" + threads);
  hm::diagnostics::Finish(kShutdownWatchdogExitCode);
  ::_exit(kShutdownWatchdogExitCode);
}

} // namespace hm
