#include "hstream/src/libs/common/ProcessGuard.h"
#include "hstream/src/libs/common/GameOperationOwner.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace op = hm::game_operation;
namespace fs = std::filesystem;
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void check(absl::Status value) {
  if (!value.ok())
    throw std::runtime_error(value.ToString());
}
template <typename T>
T take(absl::StatusOr<T> value) {
  check(value.status());
  return std::move(*value);
}
template <typename Condition>
void eventually(Condition condition, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    if (condition())
      return;
    ::poll(nullptr, 0, 10);
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error(message);
}
struct Pipe {
  int descriptors[2];
  Pipe() {
    require(::pipe(descriptors) == 0, "cannot create fixture pipe");
  }
  ~Pipe() {
    ::close(descriptors[0]);
    ::close(descriptors[1]);
  }
  template <typename T>
  void send(const T& value) {
    require(::write(descriptors[1], &value, sizeof(value)) == sizeof(value), "cannot signal fixture");
  }
  template <typename T>
  T receive() {
    struct pollfd ready {
      descriptors[0], POLLIN, 0
    };
    int status;
    do {
      status = ::poll(&ready, 1, 5000);
    } while (status < 0 && errno == EINTR);
    require(status > 0 && (ready.revents & POLLIN), "fixture did not become ready");
    T value{};
    require(::read(descriptors[0], &value, sizeof(value)) == sizeof(value), "cannot read fixture");
    return value;
  }
};
struct Child {
  pid_t pid{-1};
  ~Child() {
    if (pid <= 0)
      return;
    ::kill(pid, SIGKILL);
    while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
    }
  }
  int wait() {
    int status = 0;
    require(::waitpid(pid, &status, 0) == pid, "cannot reap fixture");
    pid = -1;
    return status;
  }
  int wait_bounded() {
    int status = 0;
    eventually(
        [&] {
          pid_t result;
          do {
            result = ::waitpid(pid, &status, WNOHANG);
          } while (result < 0 && errno == EINTR);
          require(result >= 0, "cannot reap bounded fixture");
          return result == pid;
        },
        "guard did not retire paused descendants within five seconds");
    pid = -1;
    return status;
  }
};
Child launch(const std::string& executable, const std::vector<std::string>& args, bool new_session = false) {
  std::vector<char*> argv{const_cast<char*>(executable.c_str())};
  for (const auto& argument : args)
    argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);
  const pid_t pid = ::fork();
  require(pid >= 0, "cannot fork fixture");
  if (!pid) {
    if (new_session && ::setsid() < 0)
      ::_exit(126);
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }
  return Child{pid};
}
struct Admission {
  fs::path game;
  op::Owner owner;
  explicit Admission(const fs::path& path, bool supervised = true)
      : game(path), owner(op::LocalOwner("pipeline", "guard-test", take(op::NewToken()))) {
    fs::create_directory(game);
    auto lock = take(op::Lock::Acquire(game));
    check(lock->Claim(owner));
    op::RegisterAuthorization(game, owner.token);
    if (supervised)
      check(op::EnableSupervisedChildProof(owner.token));
  }
  ~Admission() {
    op::ForgetAuthorization(game, owner.token);
  }
  op::SupervisedCommand prepare(const std::vector<std::string>& command) {
    return take(op::PrepareSupervisedCommand(game, owner.token, 1000, 10, command));
  }
  void release() {
    check(op::ReleaseLocal(game, owner.token, true));
  }
};
volatile sig_atomic_t signalled = 0;
int resume_receipt = -1;
int resume_output = -1;
off_t resume_state_offset = 0;
void signal_fixture(int) {
  signalled = 1;
}
void resumed_fixture(int) {
  char state = '?';
  if (::pread(resume_receipt, &state, 1, resume_state_offset) != 1)
    state = '!';
  if (::write(resume_output, &state, 1) != 1)
    ::_exit(126);
}

int worker(int argc, char** argv) {
  const int output = std::stoi(argv[2]);
  const pid_t own = ::getpid();
  ::signal(SIGTERM, signal_fixture);
  if (argc > 3 && std::string(argv[1]) == "--worker-wait") {
    resume_receipt = ::open(argv[3], O_RDONLY);
    struct stat metadata {};
    require(
        resume_receipt >= 0 && ::fstat(resume_receipt, &metadata) == 0 && metadata.st_size > 0,
        "cannot open fixture receipt for Resume ordering check");
    resume_output = output;
    resume_state_offset = metadata.st_size - 1;
    ::signal(SIGCONT, resumed_fixture);
  }
  require(::write(output, &own, sizeof(own)) == sizeof(own), "worker ready failed");
  if (std::string(argv[1]) == "--worker-wait") {
    for (;;)
      ::pause();
  }
  while (!signalled)
    ::pause();
  const pid_t helper = ::fork();
  require(helper >= 0, "worker fork failed");
  if (!helper) {
    require(::setsid() >= 0 && ::prctl(PR_SET_DUMPABLE, 0) == 0, "cannot detach hidden helper");
    ::signal(SIGTERM, SIG_IGN);
    const pid_t child = ::getpid();
    require(::write(output, &child, sizeof(child)) == sizeof(child), "hidden helper ready failed");
    for (;;)
      ::pause();
  }
  return 0; // The new detached child is adopted while the guard retires root.
}

char process_state(pid_t pid) {
  std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  require(bool(std::getline(input, line)), "fixture process disappeared before pause verification");
  const auto end = line.rfind(')');
  require(end != std::string::npos && end + 2 < line.size(), "invalid fixture process state");
  return line[end + 2];
}

void paused_retirement(
    const fs::path& root,
    const std::string& guard,
    const std::string& self,
    bool parent_death,
    bool during_pause = false) {
  Admission admission(
      root /
      (during_pause       ? "pause-parent-death"
           : parent_death ? "paused-parent-death"
                          : "paused-force-cancel"));
  Pipe worker_ready, guard_ready;
  auto command = admission.prepare({self, "--worker-wait", std::to_string(worker_ready.descriptors[1])});
  command.arguments.push_back(command.arguments[1]);
  Child launching_parent;
  auto process = [&]() -> Child {
    if (!parent_death)
      return launch(guard, command.arguments, true);
    launching_parent.pid = ::fork();
    require(launching_parent.pid >= 0, "cannot fork guard launching parent");
    if (!launching_parent.pid) {
      auto arguments = command.arguments;
      for (size_t index = 0; index + 1 < arguments.size(); ++index)
        if (arguments[index] == "--parent-pid")
          arguments[index + 1] = std::to_string(::getpid());
      auto supervisor = launch(guard, arguments, true);
      guard_ready.send(supervisor.pid);
      for (;;)
        ::pause();
    }
    return Child{guard_ready.receive<pid_t>()};
  }();
  check(op::SupervisedCommandStarted(admission.owner.token, command.id));
  Child survivor{worker_ready.receive<pid_t>()};
  require(
      ::getpgid(survivor.pid) == survivor.pid && ::getpgid(process.pid) != survivor.pid,
      "guard must remain outside the worker pause group");
  check(op::SignalSupervisedCommand(admission.owner.token, command.id, SIGSTOP));
  if (!during_pause) {
    eventually(
        [&] { return take(op::SupervisedCommandPaused(admission.owner.token, command.id)); },
        "guard did not acknowledge worker pause");
    require(process_state(survivor.pid) == 'T', "guard acknowledged a worker that was not stopped");
    for (int index = 0; index < 10; ++index) {
      require(process_state(process.pid) != 'T', "worker pause must never stop the guard");
      require(process_state(survivor.pid) == 'T', "worker must remain paused");
      ::poll(nullptr, 0, 10);
    }
    if (!parent_death) {
      check(op::SignalSupervisedCommand(admission.owner.token, command.id, SIGCONT));
      require(worker_ready.receive<char>() == 'R', "worker Resume cannot observe stale Pause acknowledgement");
      eventually(
          [&] {
            return !take(op::SupervisedCommandPaused(admission.owner.token, command.id)) &&
                process_state(survivor.pid) != 'T';
          },
          "guard did not resume paused worker");
      check(op::SignalSupervisedCommand(admission.owner.token, command.id, SIGSTOP));
      eventually(
          [&] { return take(op::SupervisedCommandPaused(admission.owner.token, command.id)); },
          "guard did not acknowledge repeated worker pause");
    }
  }
  require(!take(op::SupervisedCommandStopped(admission.owner.token, command.id)), "paused work cannot prove cleanup");
  if (parent_death) {
    require(::kill(launching_parent.pid, SIGKILL) == 0, "cannot kill guard launching parent");
    require(WIFSIGNALED(launching_parent.wait_bounded()), "guard launching parent must have been killed");
  } else
    check(op::SignalSupervisedCommand(admission.owner.token, command.id, SIGKILL));
  const int status = process.wait_bounded();
  require(WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGKILL, "guard must report paused worker termination");
  require(::kill(survivor.pid, 0) == -1 && errno == ESRCH, "guard must reap paused worker before completion");
  survivor.pid = -1;
  check(op::SupervisedCommandFinished(admission.owner.token, command.id));
  require(take(op::SupervisedCommandStopped(admission.owner.token, command.id)), "paused retirement proof missing");
  admission.release();
}
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc > 2 && std::string(argv[1]).rfind("--worker-", 0) == 0)
      return worker(argc, argv);
    require(argc == 2, "expected guard executable argument");
    const std::string guard = fs::absolute(argv[1]), self = fs::canonical("/proc/self/exe");
    // Test-only adoption contains a guard killed before it can write proof.
    // Production Qt neither enables global scope nor reaps unrelated children.
    require(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "cannot contain test descendants");
    char pattern[] = "/tmp/hstream-process-guard-XXXXXX";
    const char* directory = ::mkdtemp(pattern);
    require(directory, "cannot create guard fixtures");
    const fs::path root(directory);
    struct Cleanup {
      fs::path path;
      ~Cleanup() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
      }
    } cleanup{root};
    {
      Admission admission(root / "no-proof", false);
      require(!op::LocalChildrenStopped(admission.owner.token).ok(), "a process scan cannot authorize cleanup");
      require(!op::ReleaseLocal(admission.game, admission.owner.token, true).ok(), "unscoped local release must fail");
      require(take(op::Inspect(admission.game)).has_value(), "missing kernel proof must retain admission");
    }
    {
      Admission admission(root / "unlaunched");
      const auto command = admission.prepare({"/bin/true"});
      require(!take(op::LocalChildrenStopped(admission.owner.token)), "unlaunched permit must fence release");
      check(op::CancelUnlaunchedSupervisedCommand(admission.owner.token, command.id));
      admission.release();
    }
    {
      Pipe ready;
      Child unrelated{::fork()};
      require(unrelated.pid >= 0, "cannot fork unrelated child");
      if (!unrelated.pid) {
        ::prctl(PR_SET_DUMPABLE, 0);
        ready.send(::getpid());
        for (;;)
          ::pause();
      }
      require(ready.receive<pid_t>() == unrelated.pid, "unexpected unrelated child");
      Admission admission(root / "unrelated-child");
      const auto command = admission.prepare({"/bin/sh", "-c", "exit 7"});
      auto process = launch(guard, command.arguments);
      check(op::SupervisedCommandStarted(admission.owner.token, command.id));
      const int status = process.wait();
      require(WIFEXITED(status) && WEXITSTATUS(status) == 7, "guard must preserve worker exit status");
      require(take(op::LocalChildrenStopped(admission.owner.token)), "guard must acknowledge exact shutdown");
      check(op::SignalLocalChildren(admission.owner.token, SIGKILL));
      admission.release();
      require(::kill(unrelated.pid, 0) == 0, "unrelated Qt child must remain alive and unreaped");
    }
    {
      Admission admission(root / "fork-during-retirement");
      Pipe ready;
      const auto command = admission.prepare({self, "--worker-fork", std::to_string(ready.descriptors[1])});
      auto process = launch(guard, command.arguments);
      check(op::SupervisedCommandStarted(admission.owner.token, command.id));
      const pid_t worker = ready.receive<pid_t>();
      require(worker > 0, "worker identity missing");
      check(op::SignalSupervisedCommand(admission.owner.token, command.id, SIGTERM));
      Child survivor{ready.receive<pid_t>()};
      const int hidden = ::open(("/proc/" + std::to_string(survivor.pid) + "/environ").c_str(), O_RDONLY);
      require(hidden < 0 && (errno == EACCES || errno == EPERM), "detached descendant must be unreadable");
      require(
          !take(op::LocalChildrenStopped(admission.owner.token)),
          "a newly forked detached survivor must block release");
      require(
          !op::ReleaseLocal(admission.game, admission.owner.token, true).ok(),
          "fork-at-exit race cannot clear admission");
      require(
          !op::CancelUnlaunchedSupervisedCommand(admission.owner.token, command.id).ok(),
          "launched permit cannot be canceled");
      // A duplicate supervisor cannot consume the same permit while work lives.
      auto duplicate = launch(guard, command.arguments);
      const int rejected = duplicate.wait();
      require(WIFEXITED(rejected) && WEXITSTATUS(rejected) == 125, "duplicate supervisor must fail before launch");
      check(op::SignalSupervisedCommand(admission.owner.token, command.id, SIGKILL));
      const int status = process.wait();
      require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "guard must preserve completed root status");
      require(
          ::kill(survivor.pid, 0) == -1 && errno == ESRCH, "guard must reap hidden detached child before completion");
      survivor.pid = -1;
      admission.release();
    }
    {
      Admission admission(root / "killed-guard");
      Pipe ready;
      const auto command = admission.prepare({self, "--worker-wait", std::to_string(ready.descriptors[1])});
      auto process = launch(guard, command.arguments);
      check(op::SupervisedCommandStarted(admission.owner.token, command.id));
      Child survivor{ready.receive<pid_t>()};
      require(::kill(process.pid, SIGKILL) == 0, "cannot kill guard fixture");
      require(WIFSIGNALED(process.wait()), "guard must have exited abnormally");
      require(::kill(survivor.pid, 0) == 0, "fixture survivor must outlive killed guard");
      require(!take(op::LocalChildrenStopped(admission.owner.token)), "killed guard cannot prove cleanup");
      check(op::SupervisedCommandFinished(admission.owner.token, command.id));
      require(
          !op::LocalChildrenStopped(admission.owner.token).ok(),
          "lost guard must report recovery instead of pending work");
      require(
          !op::ReleaseLocal(admission.game, admission.owner.token, true).ok(),
          "missing receipt must preserve durable claim");
    }
    {
      Admission admission(root / "replaced-proof");
      const auto command = admission.prepare({"/bin/true"});
      auto process = launch(guard, command.arguments);
      check(op::SupervisedCommandStarted(admission.owner.token, command.id));
      require(WIFEXITED(process.wait()), "guard failed");
      const fs::path receipt(command.arguments[1]);
      fs::rename(receipt, receipt.string() + ".old");
      fs::copy_file(receipt.string() + ".old", receipt);
      require(!op::LocalChildrenStopped(admission.owner.token).ok(), "replaced receipt cannot authorize cleanup");
      require(
          !op::ReleaseLocal(admission.game, admission.owner.token, true).ok(),
          "proof path replacement must retain claim");
    }
    paused_retirement(root, guard, self, true);
    paused_retirement(root, guard, self, false);
    // Parent death can race the request itself; no UI-side SIGCONT is needed
    // to make cancellation visible while the guard applies worker SIGSTOP.
    paused_retirement(root, guard, self, true, true);
    {
      Admission admission(root / "old-receipt-version");
      const auto command = admission.prepare({"/bin/true"});
      // A separately packaged guard/UI revision cannot silently accept Pause
      // with the old protocol that did not isolate worker process groups.
      std::fstream receipt(command.arguments[1], std::ios::in | std::ios::out | std::ios::binary);
      const std::string old_header = "hstream-process-guard-v1\n";
      receipt.write(old_header.data(), old_header.size());
      receipt.close();
      auto process = launch(guard, command.arguments);
      check(op::SupervisedCommandStarted(admission.owner.token, command.id));
      const int status = process.wait_bounded();
      require(WIFEXITED(status) && WEXITSTATUS(status) == 125, "Old receipt version must fail before worker launch");
      require(!op::LocalChildrenStopped(admission.owner.token).ok(), "Old receipt cannot authorize cleanup");
    }
    {
      Admission admission(root / "pause-after-cancel");
      const auto command = admission.prepare({"/bin/false"});
      check(op::SignalSupervisedCommand(admission.owner.token, command.id, SIGINT));
      require(
          !op::SignalSupervisedCommand(admission.owner.token, command.id, SIGSTOP).ok(),
          "Pause must not replace queued cancellation");
      require(
          !op::SignalSupervisedCommand(admission.owner.token, command.id, SIGCONT).ok(),
          "Resume must not replace queued cancellation");
      auto process = launch(guard, command.arguments);
      check(op::SupervisedCommandStarted(admission.owner.token, command.id));
      const int status = process.wait_bounded();
      require(
          WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGINT,
          "Pause/Resume must preserve startup cancellation without launching work");
      admission.release();
    }
    {
      Admission admission(root / "many-sequential-commands");
      struct rlimit original {};
      require(::getrlimit(RLIMIT_NOFILE, &original) == 0, "cannot inspect descriptor limit");
      struct RestoreLimit {
        struct rlimit value;
        ~RestoreLimit() {
          ::setrlimit(RLIMIT_NOFILE, &value);
        }
      } restore{original};
      struct rlimit limited {
        64, original.rlim_max
      };
      require(::setrlimit(RLIMIT_NOFILE, &limited) == 0, "cannot bound descriptor fixture");
      for (int index = 0; index < 80; ++index) {
        const auto command = admission.prepare({"/bin/true"});
        auto process = launch(guard, command.arguments);
        check(op::SupervisedCommandStarted(admission.owner.token, command.id));
        const int status = process.wait();
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "sequential guarded command failed");
        require(take(op::SupervisedCommandStopped(admission.owner.token, command.id)), "exact command proof missing");
      }
      admission.release();
    }
    {
      // argv[0] can name a replaced installation path. Execute the inode the
      // caller actually verified, retaining the original name as argv[0].
      const auto receipt = root / "descriptor-execution";
      const std::string token(32, 'a'), nonce(32, 'b');
      const auto header = hm::process_guard::ReceiptHeader(token, nonce);
      std::ofstream(receipt) << header << "0P";
      require(::chmod(receipt.c_str(), 0600) == 0, "cannot prepare descriptor receipt");
      const int executable = ::open("/bin/true", O_RDONLY | O_CLOEXEC);
      require(executable >= 0, "cannot pin fixture executable");
      const auto parent = ::getpid();
      Child child;
      child.pid = ::fork();
      require(child.pid >= 0, "cannot launch descriptor fixture");
      if (!child.pid) {
        const auto result = hm::process_guard::Run(receipt, token, nonce, parent, 100, 10, {"/bin/false"}, executable);
        ::_exit(result.ok() ? *result : 125);
      }
      ::close(executable);
      const int status = child.wait_bounded();
      require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "guard executed pathname instead of pinned inode");
      const int proof = ::open(receipt.c_str(), O_RDONLY | O_CLOEXEC);
      require(proof >= 0, "cannot read descriptor retirement proof");
      require(take(hm::process_guard::ReadReceipt(proof, receipt, header)) == 'C', "descriptor task did not retire");
      ::close(proof);
    }
    std::cout << "Process guard kernel shutdown and receipt checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
