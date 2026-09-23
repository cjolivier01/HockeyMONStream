#include "hstream/src/libs/common/ProcessDiagnostics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/symbolize.h"

namespace hm::diagnostics {
namespace {
namespace fs = std::filesystem;
constexpr size_t kLogLimit = 1024 * 1024;
constexpr size_t kBreadcrumbLimit = 64 * 1024;
constexpr size_t kCrashLimit = 64 * 1024;
constexpr size_t kMapLimit = 1024 * 1024;
constexpr size_t kRetainedSessions = 24;
constexpr char kOwner[] = "hstream-process-diagnostics-v1\n";
struct Record {
  std::array<char, 4352> text{};
  size_t size{0};
  bool breadcrumb{false};
};
struct RecentBreadcrumb {
  std::array<char, 1024> text{};
  size_t size{0};
};
struct State {
  fs::path directory;
  std::atomic_flag queue_lock = ATOMIC_FLAG_INIT;
  std::array<Record, 256> queue;
  size_t head{0}, count{0};
  std::atomic_flag recent_lock = ATOMIC_FLAG_INIT;
  std::array<RecentBreadcrumb, 32> recent;
  size_t recent_next{0}, recent_count{0};
  std::atomic<unsigned> pending{0}, dropped{0};
  std::mutex wake_mutex;
  std::condition_variable wake;
  int lock{-1}, log{-1}, breadcrumbs{-1}, crash{-1}, maps{-1};
  std::chrono::steady_clock::time_point rate_epoch{};
  unsigned log_count{0}, breadcrumb_count{0}, error_count{0};
  ~State() {
    for (int fd : {lock, log, breadcrumbs, crash, maps})
      if (fd >= 0)
        ::close(fd);
  }
  size_t log_bytes{0}, breadcrumb_bytes{0};
};
std::atomic<State*> state{nullptr};
std::mutex initialization;
// Never close/reuse these descriptors after installing the signal handlers.
int crash_fd = -1, maps_fd = -1;
pid_t owner_pid = 0;
std::atomic<size_t> crash_bytes{0};
static_assert(std::atomic<size_t>::is_always_lock_free);
std::atomic_flag fatal_recorded = ATOMIC_FLAG_INIT;
struct sigaction fatal_handlers[NSIG]{};

bool write_all(int fd, const char* data, size_t size) noexcept {
  while (size) {
    const ssize_t count = ::write(fd, data, size);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return false;
    data += count;
    size -= static_cast<size_t>(count);
  }
  return true;
}

void snapshot_maps() noexcept {
  const int input = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (input < 0 || maps_fd < 0) {
    if (input >= 0)
      ::close(input);
    return;
  }
  if (::lseek(maps_fd, 0, SEEK_SET) < 0 || ::ftruncate(maps_fd, 0) != 0) {
    ::close(input);
    return;
  }
  char buffer[4096];
  size_t remaining = kMapLimit;
  while (remaining) {
    const ssize_t count = ::read(input, buffer, std::min(remaining, sizeof(buffer)));
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0 || !write_all(maps_fd, buffer, static_cast<size_t>(count)))
      break;
    remaining -= static_cast<size_t>(count);
  }
  ::close(input);
}

void crash_write(const char* message) noexcept {
  if (::getpid() != owner_pid || crash_fd < 0)
    return; // A fork-only child must never write its parent's crash report.
  if (!message) {
    ::fsync(crash_fd);
    return;
  }
  size_t length = 0;
  while (length < kCrashLimit && message[length])
    ++length;
  const size_t offset = crash_bytes.fetch_add(length, std::memory_order_relaxed);
  if (offset < kCrashLimit)
    write_all(crash_fd, message, std::min(length, kCrashLimit - offset));
}

char* number(char* output, uintptr_t value, unsigned base) noexcept {
  char reversed[3 * sizeof(uintptr_t) + 1];
  size_t count = 0;
  do {
    reversed[count++] = "0123456789abcdef"[value % base];
    value /= base;
  } while (value);
  while (count)
    *output++ = reversed[--count];
  return output;
}

void crash_timeout(int) noexcept {
  // Same last-resort behavior as Abseil's watchdog, including when the initial
  // persistent write or /proc read stalls before Abseil takes over.
  struct sigaction action{};
  action.sa_handler = SIG_DFL;
  ::sigemptyset(&action.sa_mask);
  ::sigaction(SIGABRT, &action, nullptr);
  sigset_t unblocked;
  ::sigemptyset(&unblocked);
  ::sigaddset(&unblocked, SIGABRT);
  ::sigprocmask(SIG_UNBLOCK, &unblocked, nullptr);
  ::kill(::getpid(), SIGABRT);
  ::_exit(128 + SIGABRT);
}

void fatal_signal(int signal, siginfo_t* info, void* context) noexcept {
  // Persist a minimal fault record BEFORE Abseil writes to stderr. A blocked
  // terminal/parent pipe must not hide which process and instruction failed.
  if (::getpid() == owner_pid && !fatal_recorded.test_and_set(std::memory_order_relaxed)) {
    struct sigaction watchdog{};
    watchdog.sa_handler = crash_timeout;
    ::sigemptyset(&watchdog.sa_mask);
    ::sigaction(SIGALRM, &watchdog, nullptr);
    ::alarm(5);
    char record[256];
    char* next = record;
    const auto literal = [&](const char* text) {
      while (*text)
        *next++ = *text++;
    };
    literal("fatal signal=");
    next = number(next, static_cast<unsigned>(signal), 10);
    literal(" pid=");
    next = number(next, static_cast<unsigned>(owner_pid), 10);
    literal(" tid=");
    next = number(next, static_cast<unsigned long>(::syscall(SYS_gettid)), 10);
    literal(" time=");
    struct timespec now{};
    ::clock_gettime(CLOCK_REALTIME, &now);
    next = number(next, static_cast<uintptr_t>(now.tv_sec), 10);
    literal(" address=0x");
    // si_addr is meaningful only for a kernel-delivered hardware fault, not
    // kill()/raise()/abort(), where the union contains sender identity instead.
    next = number(next, info && info->si_code > 0 ? reinterpret_cast<uintptr_t>(info->si_addr) : 0, 16);
    uintptr_t pc = 0;
#if defined(__x86_64__)
    if (context)
      pc = static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    if (context)
      pc = static_cast<ucontext_t*>(context)->uc_mcontext.pc;
#endif
    literal(" pc=0x");
    next = number(next, pc, 16);
    *next++ = '\n';
    *next = 0;
    crash_write(record);
    State* current = state.load(std::memory_order_acquire);
    if (current && !current->recent_lock.test_and_set(std::memory_order_acquire)) {
      // Fixed, preformatted records only. Never wait for a crashed producer.
      crash_write("Recent breadcrumbs (may duplicate breadcrumbs.log):\n");
      for (size_t i = 0; i < current->recent_count; ++i) {
        const auto& entry =
            current->recent
                [(current->recent_next + current->recent.size() - current->recent_count + i) % current->recent.size()];
        crash_write(entry.text.data());
      }
      current->recent_lock.clear(std::memory_order_release);
    } else {
      crash_write("Recent breadcrumb snapshot unavailable (producer interrupted)\n");
    }
    snapshot_maps();
  }
  fatal_handlers[signal].sa_sigaction(signal, info, context);
}

int private_file(const fs::path& path) {
  return ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
}

std::string build_id() {
  std::string result;
  ::dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void* output) {
        if (info->dlpi_name && *info->dlpi_name)
          return 0;
        for (size_t i = 0; i < info->dlpi_phnum; ++i) {
          const auto& segment = info->dlpi_phdr[i];
          if (segment.p_type != PT_NOTE)
            continue;
          const auto* bytes = reinterpret_cast<const unsigned char*>(info->dlpi_addr + segment.p_vaddr);
          size_t offset = 0;
          while (offset + sizeof(ElfW(Nhdr)) <= segment.p_memsz) {
            ElfW(Nhdr) note;
            std::memcpy(&note, bytes + offset, sizeof(note));
            offset += sizeof(note);
            const size_t name_size = (size_t(note.n_namesz) + 3) & ~size_t(3);
            const size_t desc_size = (size_t(note.n_descsz) + 3) & ~size_t(3);
            if (name_size + desc_size > segment.p_memsz - offset)
              break;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 && std::memcmp(bytes + offset, "GNU", 4) == 0) {
              auto& id = *static_cast<std::string*>(output);
              for (size_t n = 0; n < std::min(size_t(note.n_descsz), size_t(64)); ++n) {
                const auto value = bytes[offset + name_size + n];
                id += "0123456789abcdef"[value >> 4];
                id += "0123456789abcdef"[value & 15];
              }
              return 1;
            }
            offset += name_size + desc_size;
          }
        }
        return 1;
      },
      &result);
  return result.empty() ? "unavailable" : result;
}

struct ScopedFd {
  int fd{-1};
  ~ScopedFd() {
    if (fd >= 0)
      ::close(fd);
  }
};

void retain_recent(const fs::path& root) {
  std::vector<fs::directory_entry> entries;
  for (const auto& entry : fs::directory_iterator(root)) {
    const auto name = entry.path().filename().string();
    if (name.rfind("run-", 0) == 0 && !entry.is_symlink() && entry.is_directory())
      entries.push_back(entry);
  }
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
    return a.path().filename() > b.path().filename();
  });
  size_t finished = 0;
  for (const auto& entry : entries) {
    const int fd = ::open((entry.path() / "owner").c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
      continue;
    char marker[sizeof(kOwner)]{};
    struct stat attributes{};
    const bool owned = ::fstat(fd, &attributes) == 0 && attributes.st_uid == ::geteuid() &&
        ::read(fd, marker, sizeof(marker)) == sizeof(kOwner) - 1 &&
        std::memcmp(marker, kOwner, sizeof(kOwner) - 1) == 0;
    if (owned && ::flock(fd, LOCK_EX | LOCK_NB) == 0 && ++finished > kRetainedSessions) {
      std::error_code ignored;
      fs::remove_all(entry.path(), ignored);
    }
    ::close(fd);
  }
}

std::string bounded_text(std::string_view text, size_t limit) {
  std::string result;
  result.reserve(std::min(text.size(), limit));
  for (char c : text.substr(0, limit))
    result += (c == '\n' || c == '\r' || c == '\0') ? ' ' : c;
  if (text.size() > limit)
    result += " [truncated]";
  return result;
}

void write_record(State& current, const Record& record) noexcept {
  try {
    int& fd = record.breadcrumb ? current.breadcrumbs : current.log;
    size_t& bytes = record.breadcrumb ? current.breadcrumb_bytes : current.log_bytes;
    const auto path = current.directory / (record.breadcrumb ? "breadcrumbs.log" : "events.log");
    if (fd < 0)
      return;
    if (bytes + record.size > (record.breadcrumb ? kBreadcrumbLimit : kLogLimit)) {
      ::close(fd);
      fd = -1;
      std::error_code error;
      fs::rename(path, path.string() + ".1", error);
      if (error)
        return;
      fd = private_file(path);
      bytes = 0;
    }
    if (fd >= 0) {
      if (write_all(fd, record.text.data(), record.size))
        bytes += record.size;
      else {
        ::close(fd);
        fd = -1; // Stop after partial/error writes, keeping the byte bound valid.
      }
    }
  } catch (...) {
    // A failed diagnostic disk must not turn into an application failure.
  }
}

void writer(State* current) noexcept {
  for (;;) {
    std::array<Record, 16> batch;
    size_t count = 0;
    if (!current->queue_lock.test_and_set(std::memory_order_acquire)) {
      while (count < batch.size() && current->count) {
        batch[count++] = current->queue[current->head];
        current->head = (current->head + 1) % current->queue.size();
        --current->count;
      }
      current->queue_lock.clear(std::memory_order_release);
    }
    // All filesystem work is outside the producer guard, on this one thread.
    for (size_t i = 0; i < count; ++i) {
      write_record(*current, batch[i]);
      current->pending.fetch_sub(1, std::memory_order_release);
    }
    const unsigned dropped = current->dropped.exchange(0, std::memory_order_relaxed);
    if (dropped) {
      Record notice;
      notice.size = std::snprintf(notice.text.data(), notice.text.size(), "diagnostics dropped-records=%u\n", dropped);
      write_record(*current, notice);
    }
    if (!count) {
      std::unique_lock<std::mutex> lock(current->wake_mutex);
      // Timeout also handles a notification arriving just before wait_for.
      current->wake.wait_for(
          lock, std::chrono::milliseconds(100), [&] { return current->pending.load(std::memory_order_acquire) != 0; });
    }
  }
}

void format_record(Record& entry, std::string_view category, std::string_view message, bool breadcrumb) noexcept {
  entry.breadcrumb = breadcrumb;
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  entry.size = std::snprintf(
      entry.text.data(),
      entry.text.size(),
      "%lld tid=%ld ",
      static_cast<long long>(milliseconds),
      ::syscall(SYS_gettid));
  const auto append = [&](std::string_view value, size_t limit) {
    for (char c : value.substr(0, limit))
      entry.text[entry.size++] = (c == '\n' || c == '\r' || c == '\0') ? ' ' : c;
  };
  append(category, 80);
  entry.text[entry.size++] = ' ';
  append(message, 4096);
  if (message.size() > 4096)
    append(" [truncated]", 20);
  entry.text[entry.size++] = '\n';
  entry.text[entry.size] = 0;
}

void record(std::string_view category, std::string_view message, bool breadcrumb) noexcept {
  State* current = state.load(std::memory_order_acquire);
  if (!current || ::getpid() != owner_pid)
    return;
  // The disk writer must never make the emergency history unavailable. Keep
  // this fixed ring independent of its queue, with no waiting in either path.
  std::optional<Record> emergency_record;
  if (breadcrumb)
    emergency_record.emplace();
  if (breadcrumb) {
    auto& emergency = *emergency_record;
    format_record(emergency, category, message, true);
    if (!current->recent_lock.test_and_set(std::memory_order_acquire)) {
      auto& recent = current->recent[current->recent_next];
      recent.size = std::min(emergency.size, recent.text.size() - 2);
      std::memcpy(recent.text.data(), emergency.text.data(), recent.size);
      if (recent.size && recent.text[recent.size - 1] != '\n')
        recent.text[recent.size++] = '\n';
      recent.text[recent.size] = 0;
      current->recent_next = (current->recent_next + 1) % current->recent.size();
      current->recent_count = std::min(current->recent_count + 1, current->recent.size());
      current->recent_lock.clear(std::memory_order_release);
    }
  }
  if (current->queue_lock.test_and_set(std::memory_order_acquire)) {
    current->dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - current->rate_epoch >= std::chrono::seconds(1)) {
    current->rate_epoch = now;
    current->log_count = current->breadcrumb_count = current->error_count = 0;
  }
  const bool error = category.find("error") != std::string_view::npos ||
      category.find("stderr") != std::string_view::npos || category.find("warning") != std::string_view::npos;
  unsigned& count = breadcrumb ? current->breadcrumb_count : (error ? current->error_count : current->log_count);
  if (count >= 100 || current->count == current->queue.size()) {
    current->dropped.fetch_add(1, std::memory_order_relaxed);
    current->queue_lock.clear(std::memory_order_release);
    return;
  }
  ++count;
  auto& entry = current->queue[(current->head + current->count) % current->queue.size()];
  if (breadcrumb)
    entry = *emergency_record;
  else
    format_record(entry, category, message, false);
  ++current->count;
  current->pending.fetch_add(1, std::memory_order_relaxed);
  current->queue_lock.clear(std::memory_order_release);
  current->wake.notify_one();
}

} // namespace

bool Initialize(const char* component, const char* executable) noexcept {
  if (state.load(std::memory_order_acquire))
    return ::getpid() == owner_pid; // Before touching a possibly inherited mutex.
  try {
    std::lock_guard<std::mutex> guard(initialization);
    if (state.load())
      return ::getpid() == owner_pid;
    if (const char* disabled = std::getenv("HSTREAM_DIAGNOSTICS_DISABLE"); disabled && std::strcmp(disabled, "1") == 0)
      return false;
    fs::path root;
    if (const char* override_path = std::getenv("HSTREAM_DIAGNOSTICS_DIR"); override_path && *override_path)
      root = override_path;
    else if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg && fs::path(xdg).is_absolute())
      root = fs::path(xdg) / "hstream/diagnostics";
    else if (const char* home = std::getenv("HOME"); home && *home)
      root = fs::path(home) / ".local/state/hstream/diagnostics";
    else
      return false;
    root = fs::absolute(root);
    fs::create_directories(root);
    ScopedFd retention{::open((root / ".retention-lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600)};
    const bool maintain_history = retention.fd >= 0 && ::flock(retention.fd, LOCK_EX | LOCK_NB) == 0;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    const std::string name = bounded_text(component ? component : "process", 64);
    if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos)
      return false;
    std::string pattern = (root / ("run-" + std::to_string(nanos) + "-" + name + "-XXXXXX")).string();
    if (!::mkdtemp(pattern.data()))
      return false;
    auto current = std::make_unique<State>();
    current->directory = pattern;
    // Publish the ownership marker only after acquiring its lifetime lock.
    // Concurrent retention ignores incomplete/unmarked sessions, so a busy
    // maintenance lock can skip pruning without disabling crash reporting.
    current->lock = private_file(current->directory / "owner");
    if (current->lock < 0 || ::flock(current->lock, LOCK_EX | LOCK_NB) != 0 ||
        !write_all(current->lock, kOwner, sizeof(kOwner) - 1))
      return false;
    current->log = private_file(current->directory / "events.log");
    current->breadcrumbs = private_file(current->directory / "breadcrumbs.log");
    current->crash = private_file(current->directory / "crash.txt");
    current->maps = private_file(current->directory / "maps.txt");
    if (current->log < 0 || current->breadcrumbs < 0 || current->crash < 0 || current->maps < 0)
      return false;
    std::array<char, 4096> path{};
    const auto length = ::readlink("/proc/self/exe", path.data(), path.size() - 1);
    const std::string binary = length > 0 ? std::string(path.data(), length) : (executable ? executable : "unknown");
    const std::string metadata = "format=1\ncomponent=" + name + "\npid=" + std::to_string(::getpid()) +
        "\nparent-pid=" + std::to_string(::getppid()) + "\nexecutable=" + binary +
        "\nstarted-ns=" + std::to_string(nanos) + "\nbuild-id=" + build_id() + "\n";
    const int metadata_fd = private_file(current->directory / "process.txt");
    if (metadata_fd >= 0) {
      write_all(metadata_fd, metadata.data(), metadata.size());
      ::close(metadata_fd);
    }
    try {
      if (maintain_history)
        retain_recent(root);
    } catch (...) {
    }
    crash_fd = current->crash;
    maps_fd = current->maps;
    owner_pid = ::getpid();
    // Process-lifetime state: Qt/SDK destructors may still log during teardown.
    std::thread output_thread(writer, current.get());
    output_thread.detach();
    state.store(current.release(), std::memory_order_release);
    snapshot_maps();
    absl::InitializeSymbolizer(executable ? executable : binary.c_str());
    struct sigaction termination{};
    ::sigaction(SIGTERM, nullptr, &termination);
    absl::FailureSignalHandlerOptions options;
    options.writerfn = crash_write;
    absl::InstallFailureSignalHandler(options);
    // SIGTERM is normal runner cancellation/parent-death handling, not a crash.
    ::sigaction(SIGTERM, &termination, nullptr);
    for (const int signal : {SIGSEGV, SIGILL, SIGFPE, SIGABRT, SIGBUS, SIGTRAP}) {
      ::sigaction(signal, nullptr, &fatal_handlers[signal]);
      auto handler = fatal_handlers[signal];
      handler.sa_sigaction = fatal_signal;
      ::sigaction(signal, &handler, nullptr);
    }
    Breadcrumb("process", "started");
    return true;
  } catch (...) {
    return false;
  }
}

std::string Directory() {
  const auto* current = state.load(std::memory_order_acquire);
  return current && ::getpid() == owner_pid ? current->directory.string() : std::string();
}
void Log(std::string_view category, std::string_view message) noexcept {
  record(category, message, false);
}
void Breadcrumb(std::string_view category, std::string_view message) noexcept {
  record(category, message, true);
}
void Finish(int exit_code) noexcept {
  try {
    Breadcrumb("process-exit", std::to_string(exit_code));
    State* current = state.load(std::memory_order_acquire);
    if (!current || ::getpid() != owner_pid)
      return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (current->pending.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } catch (...) {
  }
}
} // namespace hm::diagnostics
