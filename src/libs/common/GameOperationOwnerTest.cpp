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
    {
      const auto recover_game = root / "recover";
      fs::create_directory(recover_game);
      auto owner = op::LocalOwner("pipeline", "recover-me", take(op::NewToken()));
      {
        auto lock = take(op::Lock::Acquire(recover_game));
        check(lock->Claim(owner));
      }
      const auto info = take(op::InspectRecovery(recover_game));
      require(
          info.process_matches && info.local_host && info.owner->started_unix_ms > 0,
          "new claims must retain verifiable process identity and start time");
      auto lock = take(op::Lock::Acquire(recover_game));
      require(
          !lock->Recover(owner.token, owner.revision, false, "no progress").ok(),
          "no progress is never verified retirement");
      check(lock->RequestCancel(owner.token, owner.revision));
      require(
          !lock->Recover(owner.token, owner.revision, true, "stale confirmation").ok(),
          "confirmation must not cross an owner revision");
      owner = *take(lock->Read());
      lock.reset();
      op::RegisterAuthorization(recover_game, owner.token);
      require(op::CancellationRequested(), "a remote stop intent must reach the owning timer");
      require(!op::Completion(recover_game).ok(), "cancelled owner cannot commit outputs");
      require(op::Mutation(recover_game).ok(), "cancelled owner can still perform supervised cleanup");
      const auto state = hm::game_storage::StateDirectory(recover_game);
      std::ofstream(state / "unfinished.mkv") << "retained-output";
      std::ofstream(state / "config.yaml") << "retained-config";
      lock = take(op::Lock::Acquire(recover_game));
      check(lock->Recover(owner.token, owner.revision, true, "operator abandons legacy run"));
      require(!take(lock->Read()), "explicit force release must remove the active claim");
      require(
          fs::exists(state / ("game-operation-revoked-" + owner.token + ".yaml")),
          "force release must durably preserve a revocation and audit");
      require(
          fs::file_size(state / "unfinished.mkv") == 15 && fs::file_size(state / "config.yaml") == 15,
          "recovery must preserve unfinished outputs and configuration");
      require(!lock->Authorize(owner.token).ok(), "abandoned tokens must remain revoked with no successor");
      require(!lock->Claim(owner).ok(), "an abandoned controller cannot reacquire with the same token");
      lock.reset();
      op::ForgetAuthorization(recover_game, owner.token);
      ::setenv("HSTREAM_GAME_OPERATION_GAME", recover_game.c_str(), 1);
      ::setenv("HSTREAM_GAME_OPERATION_TOKEN", owner.token.c_str(), 1);
      require(!op::Mutation(recover_game).ok(), "delayed child cannot reacquire after inherited token revocation");
      ::unsetenv("HSTREAM_GAME_OPERATION_GAME");
      ::unsetenv("HSTREAM_GAME_OPERATION_TOKEN");
      op::RegisterAuthorization(recover_game, owner.token);
      require(!op::Completion(recover_game).ok(), "revoked finalizer cannot commit without a successor");
      require(
          !op::PrepareSupervisedCommand(recover_game, owner.token, 1000, 10, {"/bin/true"}).ok(),
          "revoked owner cannot issue a later helper permit");
      lock = take(op::Lock::Acquire(recover_game));
      auto next = op::LocalOwner("pipeline", "next", take(op::NewToken()));
      check(lock->Claim(next));
      require(
          !lock->Recover(owner.token, owner.revision, true, "old dialog").ok(),
          "an old dialog cannot release the next run");
      lock.reset();
      check(op::ReleaseLocal(recover_game, owner.token, true));
      require(
          take(op::Inspect(recover_game))->token == next.token,
          "retiring a revoked local scope must preserve successor ownership");
      lock = take(op::Lock::Acquire(recover_game));
      check(lock->Release(next.token, next.revision, true));
      auto rebooted = op::LocalOwner("pipeline", "previous-boot", take(op::NewToken()));
      rebooted.boot_id = "previous-boot";
      check(lock->Claim(rebooted));
      check(lock->Recover(rebooted.token, rebooted.revision, false, "local reboot verified"));
      auto workflow = op::LocalOwner("workflow", "remote-tasks", take(op::NewToken()));
      workflow.boot_id = "previous-boot";
      check(lock->Claim(workflow));
      require(
          !lock->Recover(workflow.token, workflow.revision, false, "local reboot").ok(),
          "controller reboot does not prove its remote tasks stopped");
      check(lock->Release(workflow.token, workflow.revision, true));
      auto blocked = op::LocalOwner("pipeline", "bad-audit", take(op::NewToken()));
      check(lock->Claim(blocked));
      fs::create_symlink(state / "config.yaml", state / ("game-operation-revoked-" + blocked.token + ".yaml"));
      require(
          !lock->Recover(blocked.token, blocked.revision, true, "bad audit").ok() && take(lock->Read()).has_value(),
          "audit publication failure must preserve ownership and reject symlinks");
    }
    {
      const auto legacy = root / "legacy";
      fs::create_directory(legacy);
      auto lock = take(op::Lock::Acquire(legacy));
      const auto state = hm::game_storage::StateDirectory(legacy);
      const std::string contents =
          "schema: 1\ngame_admission: exclusive\nkind: pipeline\nid: old\n"
          "token: old-token\nhost: remote-host\nboot_id: old-boot\nworkflow_directory: ''\n"
          "process_id: 9999999\nrevision: 1\ncancel_requested: false\n";
      std::ofstream(state / "game-operation.yaml") << contents;
      auto old = *take(lock->Read());
      require(old.started_unix_ms == 0 && old.process_start_ticks == 0, "legacy unknown identity stays unknown");
      lock.reset();
      const auto info = take(op::InspectRecovery(legacy));
      require(!info.verified_reboot && !info.process_matches, "remote missing PID cannot verify shutdown");
      lock = take(op::Lock::Acquire(legacy));
      check(lock->Recover(old.token, old.revision, true, "legacy recovery"));
      // Simulate interruption after the audit was committed but before unlink.
      std::ofstream(state / "game-operation.yaml") << contents;
      lock.reset();
      op::RegisterAuthorization(legacy, old.token);
      require(op::CancellationRequested(), "committed revocation cancels even before owner unlink");
      op::ForgetAuthorization(legacy, old.token);
      lock = take(op::Lock::Acquire(legacy));
      check(lock->Recover(old.token, old.revision, true, "retry interrupted recovery"));
      auto reused = op::LocalOwner("pipeline", "reused-pid", take(op::NewToken()));
      ++reused.process_start_ticks;
      check(lock->Claim(reused));
      lock.reset();
      require(!take(op::InspectRecovery(legacy)).process_matches, "PID reuse cannot identify the owner");
    }
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
