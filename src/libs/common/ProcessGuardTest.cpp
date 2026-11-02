#include "hstream/src/libs/common/GameOperationOwner.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
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
    struct pollfd ready{descriptors[0], POLLIN, 0};
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
};
Child launch(const std::string& executable, const std::vector<std::string>& args) {
  std::vector<char*> argv{const_cast<char*>(executable.c_str())};
  for (const auto& argument : args)
    argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);
  const pid_t pid = ::fork();
  require(pid >= 0, "cannot fork fixture");
  if (!pid) {
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
void signal_fixture(int) {
  signalled = 1;
}

int worker(int argc, char** argv) {
  const int output = std::stoi(argv[2]);
  const pid_t own = ::getpid();
  ::signal(SIGTERM, signal_fixture);
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
    {
      Admission admission(root / "many-sequential-commands");
      struct rlimit original{};
      require(::getrlimit(RLIMIT_NOFILE, &original) == 0, "cannot inspect descriptor limit");
      struct RestoreLimit {
        struct rlimit value;
        ~RestoreLimit() {
          ::setrlimit(RLIMIT_NOFILE, &value);
        }
      } restore{original};
      struct rlimit limited{64, original.rlim_max};
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
    std::cout << "Process guard kernel shutdown and receipt checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
