#include "hstream/src/libs/common/GameOperationOwner.h"
#include "hstream/src/libs/common/GameStorage.h"

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
    check(op::EnableIsolatedChildProof());
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
      require(
          ::waitpid(child, nullptr, WNOHANG) == -1 && errno == ECHILD,
          "isolated kernel proof must reap the exited helper");
      require(take(op::LocalChildrenStopped(owner.token)), "reaped helper must permit confirmed cleanup");
      check(op::ReleaseLocal(game, owner.token, true));
      require(!take(op::Inspect(game)), "local release must clear the durable owner after child proof");
    }
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
