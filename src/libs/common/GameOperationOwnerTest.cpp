#include "hstream/src/libs/common/GameOperationOwner.h"
#include "hstream/src/libs/common/GameStorage.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
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
template <typename T>
T take(absl::StatusOr<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().ToString());
  return std::move(*result);
}
void check(absl::Status status) {
  if (!status.ok())
    throw std::runtime_error(status.ToString());
}
void competing_process(const fs::path& game, const std::string& kind) {
  const pid_t child = ::fork();
  require(child >= 0, "fork failed");
  if (child == 0) {
    auto lock = op::Lock::Acquire(game);
    if (!lock.ok())
      ::_exit(2);
    auto owner = op::LocalOwner(kind, "other", "other-token");
    ::_exit(absl::IsAlreadyExists((*lock)->Claim(owner)) ? 0 : 3);
  }
  int status = 0;
  require(
      ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
      "second process bypassed active admission");
}

struct Pipe {
  int descriptors[2];
  Pipe() {
    require(::pipe2(descriptors, O_CLOEXEC) == 0, "cannot create process fixture pipe");
  }
  ~Pipe() {
    ::close(descriptors[0]);
    ::close(descriptors[1]);
  }
  template <typename T>
  void send(const T& value) const {
    require(::write(descriptors[1], &value, sizeof(value)) == sizeof(value), "cannot signal process fixture");
  }
  template <typename T>
  T receive() const {
    struct pollfd ready{descriptors[0], POLLIN, 0};
    int result;
    do {
      result = ::poll(&ready, 1, 5000);
    } while (result < 0 && errno == EINTR);
    require(result > 0 && (ready.revents & POLLIN), "process fixture did not become ready");
    T value{};
    require(::read(descriptors[0], &value, sizeof(value)) == sizeof(value), "cannot read process fixture");
    return value;
  }
};

struct Child {
  pid_t pid;
  ~Child() {
    if (pid <= 0)
      return;
    ::kill(pid, SIGKILL);
    while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
    }
  }
  void reap() {
    int status = 0;
    require(
        ::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "process fixture failed");
    pid = -1;
  }
};

uint64_t current_ticks() {
  struct timespec now{};
  const long rate = ::sysconf(_SC_CLK_TCK);
  require(rate > 0 && ::clock_gettime(CLOCK_BOOTTIME, &now) == 0, "cannot inspect process admission clock");
  return uint64_t(now.tv_sec) * rate + uint64_t(now.tv_nsec) * rate / 1000000000ULL;
}

void next_tick() {
  const auto before = current_ticks();
  for (int retry = 0; retry < 1000; ++retry) {
    if (current_ticks() > before)
      return;
    ::usleep(1000);
  }
  throw std::runtime_error("process admission clock did not advance");
}

[[noreturn]] void hidden_helper(const Pipe& ready, pid_t parent) {
  try {
    require(::prctl(PR_SET_PDEATHSIG, SIGKILL) == 0 && ::getppid() == parent, "cannot contain hidden process fixture");
    require(::setsid() >= 0 && ::prctl(PR_SET_DUMPABLE, 0) == 0, "cannot hide process fixture environment");
    ready.send(::getpid());
    for (;;)
      ::pause();
  } catch (...) {
    ::_exit(90);
  }
}

void verify_hidden(pid_t pid) {
  const int fd = ::open(("/proc/" + std::to_string(pid) + "/environ").c_str(), O_RDONLY | O_CLOEXEC);
  const int error = errno;
  if (fd >= 0)
    ::close(fd);
  require(fd < 0 && (error == EACCES || error == EPERM), "fixture must have an unreadable environment");
}

op::Owner claim_local(const fs::path& game) {
  fs::create_directory(game);
  auto owner = op::LocalOwner("pipeline", "hidden-helper", take(op::NewToken()));
  auto lock = take(op::Lock::Acquire(game));
  check(lock->Claim(owner));
  return owner;
}

void hidden_service_test(const fs::path& game, bool preexisting) {
  const auto owner = claim_local(game);
  if (!preexisting) {
    op::RegisterAuthorization(game, owner.token);
    next_tick();
  }
  Pipe command, ready;
  Child service{::fork()};
  require(service.pid >= 0, "cannot fork unrelated service");
  if (service.pid == 0) {
    try {
      ready.send(::getpid());
      require(command.receive<char>() == 's', "invalid service fixture command");
      const pid_t parent = ::getpid();
      const pid_t helper = ::fork();
      require(helper >= 0, "cannot fork hidden service helper");
      if (helper == 0)
        hidden_helper(ready, parent);
      int status = 0;
      require(::waitpid(helper, &status, 0) == helper && WIFSIGNALED(status), "service helper was not retired");
      ::_exit(0);
    } catch (...) {
      ::_exit(91);
    }
  }
  require(ready.receive<pid_t>() == service.pid, "wrong service identity");
  next_tick();
  // Re-registration must retain the original admission epoch and ancestors.
  op::RegisterAuthorization(game, owner.token);
  command.send('s');
  Child helper{ready.receive<pid_t>()};
  verify_hidden(helper.pid);
  if (preexisting) {
    require(take(op::LocalChildrenStopped(owner.token)), "unrelated older service cannot block cleanup");
    check(op::SignalLocalChildren(owner.token, SIGKILL));
    require(::kill(helper.pid, 0) == 0, "unrelated hidden helper must not receive an owned signal");
    check(op::ReleaseLocal(game, owner.token, true));
    require(!take(op::Inspect(game)), "unrelated hidden helper must permit owner release");
  } else {
    require(!op::LocalChildrenStopped(owner.token).ok(), "re-registration cannot exclude a possible owned helper");
    require(!op::ReleaseLocal(game, owner.token, true).ok(), "unknown newer service must retain ownership");
  }
  require(::kill(helper.pid, SIGKILL) == 0, "cannot stop hidden service helper");
  helper.pid = -1; // The service owns waitpid and must reap its child.
  service.reap();
  if (!preexisting)
    check(op::ReleaseLocal(game, owner.token, true));
}

void hidden_owned_test(const fs::path& game, bool adopted) {
  int original_subreaper = 0;
  require(::prctl(PR_GET_CHILD_SUBREAPER, &original_subreaper) == 0, "cannot inspect subreaper setting");
  struct RestoreSubreaper {
    int value;
    ~RestoreSubreaper() {
      ::prctl(PR_SET_CHILD_SUBREAPER, value);
    }
  } restore{original_subreaper};
  if (adopted)
    require(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "cannot enable fixture subreaper");
  const auto owner = claim_local(game);
  op::RegisterAuthorization(game, owner.token);
  Pipe ready, exit_intermediate;
  const pid_t original_parent = ::getpid();
  Child child{::fork()};
  require(child.pid >= 0, "cannot fork hidden owned helper");
  if (child.pid == 0) {
    if (!adopted)
      hidden_helper(ready, original_parent);
    try {
      const pid_t helper = ::fork();
      require(helper >= 0, "cannot fork orphaned helper");
      if (helper == 0) {
        // It deliberately survives its intermediate parent to exercise the
        // original subreaper path; the outer fixture will own and reap it.
        require(::setsid() >= 0 && ::prctl(PR_SET_DUMPABLE, 0) == 0, "cannot hide orphan fixture");
        ready.send(::getpid());
        for (;;)
          ::pause();
      }
      require(exit_intermediate.receive<char>() == 'x', "invalid intermediate fixture command");
      ::_exit(0);
    } catch (...) {
      ::_exit(92);
    }
  }
  const pid_t hidden = ready.receive<pid_t>();
  Child orphan{adopted ? hidden : -1};
  if (adopted) {
    exit_intermediate.send('x');
    child.reap();
    siginfo_t status{};
    require(
        ::waitid(P_PID, hidden, &status, WEXITED | WNOHANG) == 0 && status.si_pid == 0,
        "hidden helper must be adopted by the original subreaper");
  }
  verify_hidden(hidden);
  require(!op::LocalChildrenStopped(owner.token).ok(), "hidden owned helper must leave cleanup uncertain");
  require(!op::ReleaseLocal(game, owner.token, true).ok(), "hidden owned helper must retain game admission");
  require(take(op::Inspect(game))->token == owner.token, "uncertain cleanup must retain exact owner");
  require(::kill(hidden, SIGKILL) == 0 && ::waitpid(hidden, nullptr, 0) == hidden, "cannot retire hidden owned helper");
  (adopted ? orphan : child).pid = -1;
  check(op::ReleaseLocal(game, owner.token, true));
}
} // namespace

int main() {
  char pattern[] = "/tmp/hstream-game-operation-XXXXXX";
  const char* temporary = ::mkdtemp(pattern);
  if (!temporary)
    return 1;
  const fs::path root(temporary);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{root};
  try {
    for (const std::string kind : {"pipeline", "int8", "workflow"}) {
      const fs::path game = root / kind;
      fs::create_directory(game);
      auto owner = op::LocalOwner(kind, "first", take(op::NewToken()));
      {
        auto lock = take(op::Lock::Acquire(game));
        check(lock->Claim(owner));
        require(owner.revision == 1, "first claim must have a revision");
        require(!op::Lock::Acquire(game, false).ok(), "admission lock must exclude competing open descriptions");
      }
      competing_process(game, kind == "workflow" ? "pipeline" : "workflow");
      require(!op::Mutation(game).ok(), "unrelated game mutation must be refused");
      op::RegisterAuthorization(game, owner.token);
      require(op::Mutation(game).ok(), "exact owner must retain config access");
      op::ForgetAuthorization(game, owner.token);
      auto lock = take(op::Lock::Acquire(game));
      require(!lock->Release(owner.token, owner.revision, false).ok(), "missing shutdown proof must retain owner");
      auto successor = op::LocalOwner(kind, owner.id, take(op::NewToken()));
      check(lock->Handoff(successor, owner.token, owner.revision));
      require(!lock->Release(owner.token, owner.revision, true).ok(), "old owner must not release successor");
      check(lock->RequestCancel(successor.token, successor.revision));
      auto cancelled = take(lock->Read());
      require(cancelled && cancelled->cancel_requested, "cancellation must be durable");
      require(!lock->Handoff(owner, cancelled->token, cancelled->revision).ok(), "cancelled owner cannot hand off");
      check(lock->Release(cancelled->token, cancelled->revision, true));
      require(!take(lock->Read()), "confirmed release must clear owner");
    }
    {
      const auto game = root / "detached-child";
      fs::create_directory(game);
      auto owner = op::LocalOwner("int8", "local", take(op::NewToken()));
      {
        auto lock = take(op::Lock::Acquire(game));
        check(lock->Claim(owner));
      }
      op::RegisterAuthorization(game, owner.token);
      const pid_t child = ::fork();
      require(child >= 0, "cannot fork child containment fixture");
      if (child == 0) {
        ::setsid();
        ::setenv("HSTREAM_GAME_OPERATION_TOKEN", owner.token.c_str(), 1);
        ::execl("/bin/sleep", "sleep", "60", nullptr);
        ::_exit(127);
      }
      bool seen = false;
      for (int retry = 0; retry < 100 && !seen; ++retry) {
        seen = !take(op::LocalChildrenStopped(owner.token));
        if (!seen)
          ::usleep(10000);
      }
      require(seen, "detached child capability must remain visible outside its parent session");
      require(!op::ReleaseLocal(game, owner.token, true).ok(), "active detached helper must retain game admission");
      check(op::SignalLocalChildren(owner.token, SIGKILL));
      siginfo_t exited{};
      require(
          ::waitid(P_PID, child, &exited, WEXITED | WNOWAIT) == 0, "observe exited helper without reaping its zombie");
      require(
          take(op::LocalChildrenStopped(owner.token)),
          "exited zombie has released resources and cannot block admission cleanup");
      int status = 0;
      require(
          ::waitpid(child, &status, 0) == child && WIFSIGNALED(status), "owned child must be signalled through pidfd");
      require(take(op::LocalChildrenStopped(owner.token)), "reaped helper must permit confirmed cleanup");
      check(op::ReleaseLocal(game, owner.token, true));
      require(!take(op::Inspect(game)), "local release must clear the durable owner after child proof");
    }
    hidden_service_test(root / "unrelated-hidden-child", true);
    hidden_service_test(root / "newer-hidden-service", false);
    hidden_owned_test(root / "owned-hidden-child", false);
    hidden_owned_test(root / "orphaned-hidden-child", true);
    const fs::path game = root / "remote";
    fs::create_directory(game);
    auto owner = op::LocalOwner("workflow", "remote-workflow", take(op::NewToken()));
    owner.host = "another-host";
    owner.boot_id = "another-boot";
    owner.process_id = 9999999;
    {
      auto lock = take(op::Lock::Acquire(game));
      check(lock->Claim(owner));
    }
    require(
        take(op::Inspect(game))->host == "another-host" && !op::Mutation(game).ok(),
        "absent local PID cannot clear remote ownership");
    const fs::path state = hm::game_storage::StateDirectory(game);
    fs::remove(state / "game-operation.yaml");
    fs::create_symlink("/etc/passwd", state / "game-operation.yaml");
    require(!op::Inspect(game).ok(), "owner symlinks must fail closed");
    fs::remove(state / "game-operation.yaml");
    std::ofstream(state / "game-operation.yaml") << "schema: 999\n";
    require(!op::Mutation(game).ok(), "unsupported ownership cannot authorize mutation");
    std::cout << "Game operation admission checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
