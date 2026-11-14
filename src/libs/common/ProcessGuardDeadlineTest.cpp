#include "hstream/src/libs/common/ProcessGuard.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
enum class Injection { None, Interrupted, Reapable, AfterRoot };
Injection injection = Injection::None;
Clock::time_point injection_end;
bool root_observed = false;
int injected_reads = 0;
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
void clean() {
  injection = Injection::None;
  const auto stopped = hm::process_guard::Reap(Clock::now() + 1s);
  require(stopped.ok() && *stopped, "fixture children did not retire");
}
void retirement(Injection mode) {
  injection = mode;
  injected_reads = 0;
  const auto start = Clock::now();
  injection_end = start + 1s;
  const auto result = hm::process_guard::Retire(20, 1);
  require(absl::IsDeadlineExceeded(result), "missing shutdown proof must retain ownership");
  require(Clock::now() - start < 300ms, "repeated waitpid responses exceeded the 40 ms budget");
  require(injected_reads > 1, "fault injection did not exercise repeated observations");
  clean();
}
} // namespace

extern "C" pid_t __real_waitpid(pid_t, int*, int);
extern "C" pid_t __wrap_waitpid(pid_t pid, int* status, int options) {
  if (injection == Injection::AfterRoot && !root_observed) {
    const auto child = __real_waitpid(pid, status, options);
    if (child > 0 && status && (WIFEXITED(*status) || WIFSIGNALED(*status))) {
      root_observed = true;
      injection_end = Clock::now() + 1s;
    }
    return child;
  }
  if (pid == -1 && injection != Injection::None && Clock::now() < injection_end) {
    ++injected_reads;
    std::this_thread::sleep_for(1ms);
    if (injection == Injection::Reapable) {
      if (status)
        *status = 0;
      return 1234567;
    }
    errno = EINTR;
    return -1;
  }
  return __real_waitpid(pid, status, options);
}

int main() {
  try {
    require(hm::process_guard::Enable().ok(), "cannot establish test subreaper");
    retirement(Injection::Interrupted);
    retirement(Injection::Reapable);
    injection = Injection::Interrupted;
    injection_end = Clock::now() + 1s;
    injected_reads = 0;
    const auto absent = hm::process_guard::Reap(Clock::now());
    require(absent.ok() && !*absent && injected_reads == 1, "expired proof read must not retry");
    clean();
    require(hm::process_guard::Signal(SIGTERM, Clock::now()).ok(), "expired signal pass should yield");

    char pattern[] = "/tmp/process-guard-deadline-XXXXXX";
    const int descriptor = ::mkstemp(pattern);
    require(descriptor >= 0, "cannot create guard permit");
    const std::string token(32, 'a'), nonce(32, 'b');
    const auto header = hm::process_guard::ReceiptHeader(token, nonce);
    const auto contents = header + "0P";
    require(
        ::write(descriptor, contents.data(), contents.size()) == static_cast<ssize_t>(contents.size()),
        "cannot write guard permit");
    injection = Injection::AfterRoot;
    const auto start = Clock::now();
    const auto result = hm::process_guard::Run(pattern, token, nonce, ::getppid(), 20, 1, {"/bin/true"});
    require(!result.ok() && absl::IsDeadlineExceeded(result.status()), "guard accepted unproved child retirement");
    require(root_observed, "guard did not observe its root status");
    require(Clock::now() - start < 300ms, "guard reaping starved retirement supervision");
    const auto receipt = hm::process_guard::ReadReceipt(descriptor, pattern, header);
    require(receipt.ok() && *receipt == 'F', "unproved guard retirement must not publish completion");
    ::close(descriptor);
    std::filesystem::remove(pattern);
    clean();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
