#include "hstream/src/libs/common/ProcessDiagnostics.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
std::string read(const fs::path& file) {
  std::ifstream input(file);
  return {std::istreambuf_iterator<char>(input), {}};
}
std::vector<fs::path> sessions(const fs::path& root) {
  std::vector<fs::path> result;
  for (const auto& entry : fs::directory_iterator(root))
    if (entry.is_directory() && entry.path().filename().string().rfind("run-", 0) == 0)
      result.push_back(entry.path());
  return result;
}
__attribute__((noinline)) void crash_leaf() {
  // Keep a non-leaf frame on ARM64 too; production vendor frames can differ.
  volatile pid_t pid = getpid();
  (void)pid;
  volatile uintptr_t fault_address = 0x150;
  *reinterpret_cast<volatile int*>(fault_address) = 1;
}
__attribute__((noinline)) void crash_parent() {
  crash_leaf();
}
int probe(const std::string& mode, const char* root, const char* executable) {
  setenv("HSTREAM_DIAGNOSTICS_DIR", root, 1);
  unsetenv("HSTREAM_DIAGNOSTICS_DISABLE");
  if (mode == "disabled")
    setenv("HSTREAM_DIAGNOSTICS_DISABLE", "1", 1);
  const bool ready = hm::diagnostics::Initialize("test", executable);
  if (mode == "unwritable" || mode == "disabled")
    return ready ? 9 : 0;
  require(ready, "initialize failed");
  hm::diagnostics::Breadcrumb("action", "before-probe");
  hm::diagnostics::Log("log", "before-probe");
  if (mode == "segv")
    crash_parent();
  if (mode == "worker") {
    std::thread worker(crash_parent);
    worker.join();
  }
  if (mode == "abort")
    std::abort();
  if (mode == "term")
    raise(SIGTERM);
  if (mode == "int")
    raise(SIGINT);
  if (mode == "fork") {
    const pid_t child = fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
      hm::diagnostics::Breadcrumb("child", "MUST-NOT-APPEAR");
      hm::diagnostics::Log("child", "MUST-NOT-APPEAR");
      require(!hm::diagnostics::Initialize("child", executable), "child reused logger");
      require(hm::diagnostics::Directory().empty(), "child reused directory");
      raise(SIGABRT);
      _exit(10);
    }
    int status = 0;
    waitpid(child, &status, 0);
    require(WIFSIGNALED(status), "child did not crash");
  }
  if (mode == "rotate") {
    for (int round = 0; round < 4; ++round) {
      for (int line = 0; line < 100; ++line) {
        hm::diagnostics::Log("rotate", std::string(6000, 'a'));
        hm::diagnostics::Breadcrumb("rotate", std::string(6000, 'b'));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    }
    hm::diagnostics::Breadcrumb("action", "after-rotation");
  }
  if (mode == "blocked-writer") {
    hm::diagnostics::Finish(0);
    int fds[2];
    require(pipe(fds) == 0, "writer pipe failed");
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    const std::string fill(4096, 'x');
    while (write(fds[1], fill.data(), fill.size()) > 0) {
    }
    fcntl(fds[1], F_SETFL, 0);
    bool replaced = false;
    for (const auto& file : fs::directory_iterator("/proc/self/fd")) {
      std::error_code error;
      if (fs::read_symlink(file.path(), error) == fs::path(hm::diagnostics::Directory()) / "events.log") {
        require(dup2(fds[1], std::stoi(file.path().filename())) >= 0, "replace writer failed");
        replaced = true;
        break;
      }
    }
    require(replaced, "writer descriptor not found");
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; ++i)
      hm::diagnostics::Log("probe", "writer is blocked, producer must not wait");
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    std::cout << "100000 calls with blocked disk writer: " << micros << " us\n" << std::flush;
    hm::diagnostics::Breadcrumb("probe", "last-before-blocked-writer-crash");
    hm::diagnostics::Finish(0);
    require(std::chrono::steady_clock::now() - started < std::chrono::seconds(1), "producer/Finish blocked on disk");
    crash_parent();
  }
  if (mode == "hold") {
    std::ofstream(fs::path(root) / "ready") << getpid();
    for (;;)
      pause();
  }
  hm::diagnostics::Finish(0);
  return 0;
}
pid_t spawn(const char* executable, const std::string& mode, const fs::path& root, int stderr_fd = -1) {
  const pid_t child = fork();
  require(child >= 0, "fork failed");
  if (child == 0) {
    struct rlimit cores{0, 0};
    setrlimit(RLIMIT_CORE, &cores);
    if (stderr_fd >= 0)
      dup2(stderr_fd, STDERR_FILENO);
    else {
      const int quiet = open("/dev/null", O_WRONLY);
      dup2(quiet, STDERR_FILENO);
    }
    execl(executable, executable, "--probe", mode.c_str(), root.c_str(), nullptr);
    _exit(127);
  }
  return child;
}
int wait(pid_t child) {
  for (int i = 0; i < 120; ++i) {
    int status = 0;
    if (waitpid(child, &status, WNOHANG) == child)
      return status;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  kill(child, SIGKILL);
  waitpid(child, nullptr, 0);
  throw std::runtime_error("probe hung");
}
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 4 && std::string(argv[1]) == "--probe")
      return probe(argv[2], argv[3], argv[0]);
    char temporary[] = "/tmp/hstream-diagnostics-test-XXXXXX";
    require(mkdtemp(temporary), "mkdtemp failed");
    const fs::path root = temporary;
    for (const auto& mode : {"segv", "worker", "abort", "term", "int", "fork", "rotate"}) {
      const auto folder = root / mode;
      const int status = wait(spawn(argv[0], mode, folder));
      const auto runs = sessions(folder);
      require(runs.size() == 1, "wrong session count");
      const auto& run = runs.front();
      const auto crash = read(run / "crash.txt");
      const auto events = read(run / "breadcrumbs.log");
      const std::string name = mode;
      if (name == "segv" || name == "worker" || name == "abort") {
        require(WIFSIGNALED(status) && WTERMSIG(status) == (name == "abort" ? SIGABRT : SIGSEGV), "wrong crash status");
        require(crash.find("fatal signal=") != std::string::npos, "missing persistent marker");
        require(crash.find("PC:") != std::string::npos, "missing native trace");
        if (name == "segv" || name == "worker")
          require(crash.find("crash_parent") != std::string::npos, "missing caller stack frame");
        require(read(run / "maps.txt").find("process_diagnostics_test") != std::string::npos, "missing process maps");
        require(crash.find("before-probe") != std::string::npos, "missing preceding breadcrumb");
      } else {
        require(crash.empty(), "normal signal or child polluted crash report");
        if (name == "term" || name == "int")
          require(
              WIFSIGNALED(status) && WTERMSIG(status) == (name == "term" ? SIGTERM : SIGINT), "normal signal changed");
        else
          require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "probe failed");
      }
      require(events.find("MUST-NOT-APPEAR") == std::string::npos, "fork wrote parent log");
      require(
          read(run / "process.txt").find("build-id=unavailable") == std::string::npos, "missing executable identity");
      struct stat attributes{};
      require(
          stat((run / "crash.txt").c_str(), &attributes) == 0 && (attributes.st_mode & 0777) == 0600,
          "file permissions");
      require(stat(run.c_str(), &attributes) == 0 && (attributes.st_mode & 0777) == 0700, "directory permissions");
      if (name == "rotate") {
        require(fs::exists(run / "events.log.1") && fs::exists(run / "breadcrumbs.log.1"), "rotation missing");
        require(events.find("after-rotation") != std::string::npos, "lost recent breadcrumb");
        for (const auto& file : {"events.log", "events.log.1", "breadcrumbs.log", "breadcrumbs.log.1"})
          require(
              fs::file_size(run / file) <= (std::string(file).find("events") == 0 ? 1024 * 1024 : 64 * 1024),
              "unbounded log");
      }
    }
    // A full stderr pipe must still leave evidence and terminate under watchdog.
    int pipe_fds[2];
    require(pipe(pipe_fds) == 0, "pipe failed");
    fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK);
    std::string fill(4096, 'x');
    while (write(pipe_fds[1], fill.data(), fill.size()) > 0) {
    }
    fcntl(pipe_fds[1], F_SETFL, 0);
    const auto blocked = root / "blocked";
    const int blocked_status = wait(spawn(argv[0], "segv", blocked, pipe_fds[1]));
    require(WIFSIGNALED(blocked_status), "blocked stderr did not terminate");
    require(
        read(sessions(blocked).front() / "crash.txt").find("fatal signal=") != std::string::npos,
        "blocked stderr hid crash");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    const auto blocked_writer = root / "blocked-writer";
    const int writer_status = wait(spawn(argv[0], "blocked-writer", blocked_writer));
    require(WIFSIGNALED(writer_status) && WTERMSIG(writer_status) == SIGSEGV, "blocked writer probe failed");
    require(
        read(sessions(blocked_writer).front() / "crash.txt").find("last-before-blocked-writer-crash") !=
            std::string::npos,
        "blocked writer lost recent breadcrumb");
    const auto contended = root / "contended";
    fs::create_directory(contended);
    const int held_lock = open((contended / ".retention-lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    require(held_lock >= 0 && flock(held_lock, LOCK_EX) == 0, "test retention lock failed");
    require(wait(spawn(argv[0], "contended", contended)) == 0, "retention lock stalled startup");
    close(held_lock);
    require(wait(spawn(argv[0], "unwritable", "/proc/hstream-cannot-create")) == 0, "filesystem failure broke startup");
    require(wait(spawn(argv[0], "disabled", root / "disabled")) == 0, "disable failed");
    require(!fs::exists(root / "disabled"), "disabled logger wrote files");
    const auto retention = root / "retention";
    const pid_t active = spawn(argv[0], "hold", retention);
    for (int i = 0; i < 100 && !fs::exists(retention / "ready"); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto active_run = sessions(retention).front();
    fs::create_directory(retention / "run-000-unowned");
    for (int i = 0; i < 28; ++i)
      require(wait(spawn(argv[0], "normal", retention)) == 0, "retention probe failed");
    // Cleanup happens at startup: current + 24 inactive + held active + unowned.
    require(sessions(retention).size() == 27, "retention limit wrong");
    require(fs::exists(active_run / "process.txt"), "active session pruned");
    kill(active, SIGTERM);
    wait(active);
    fs::remove_all(root);
    std::cout << "Process diagnostics tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
