#include "src/apps/hstream-ui/SupervisedProcess.h"

#include <signal.h>
#include <unistd.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <functional>
#include <iostream>
#include <stdexcept>

#include "hstream/src/libs/common/GameOperationOwner.h"

namespace {
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
bool wait(const std::function<bool()>& condition, int milliseconds = 10000) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < milliseconds) {
    QCoreApplication::processEvents();
    if (condition())
      return true;
    QThread::msleep(5);
  }
  return false;
}
std::string claim(const std::filesystem::path& game) {
  std::filesystem::create_directories(game);
  const auto token = hm::game_operation::NewToken();
  require(token.ok(), "Cannot create operation token");
  auto owner = hm::game_operation::LocalOwner("experiment", *token, *token);
  {
    auto lock = hm::game_operation::Lock::Acquire(game);
    require(lock.ok() && (*lock)->Claim(owner).ok(), "Cannot claim test operation");
  }
  hm::game_operation::RegisterAuthorization(game, *token);
  require(hm::game_operation::EnableSupervisedChildProof(*token).ok(), "Cannot enable command receipt proof");
  return *token;
}
void configure(QProcess& process, const std::filesystem::path& game, const std::string& token) {
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("HSTREAM_GAME_OPERATION_GAME", QString::fromStdString(game.string()));
  environment.insert("HSTREAM_GAME_OPERATION_TOKEN", QString::fromStdString(token));
  process.setProcessEnvironment(environment);
}
void check_workspace(const QProcess& process, const std::filesystem::path& game) {
  auto owner = hm::game_operation::Inspect(game);
  require(owner.ok() && owner->has_value(), "Command workspace was not reserved before launch");
  const auto environment = process.processEnvironment();
  require(
      environment.value("HSTREAM_GAME_OPERATION_GAME").toStdString() == game.string() &&
          environment.value("HSTREAM_GAME_OPERATION_TOKEN").toStdString() == (**owner).token,
      "Child did not inherit the exact private workspace admission");
}
void check_released(const std::filesystem::path& game) {
  const auto owner = hm::game_operation::Inspect(game);
  require(owner.ok() && !owner->has_value(), "Verified command left a private workspace owner behind");
}
const QString kDetached = R"PY(import ctypes,os,signal,sys,time
r,w=os.pipe()
pid=os.fork()
if pid==0:
 os.close(r); os.setsid(); os.environ.clear()
 ctypes.CDLL(None).prctl(4,0,0,0,0)
 signal.signal(signal.SIGTERM,signal.SIG_IGN); signal.signal(signal.SIGINT,signal.SIG_IGN)
 with open(sys.argv[1],'w') as f: f.write(str(os.getpid()))
 os.write(w,b'1'); os.close(w)
 while True: time.sleep(.1)
os.close(w); os.read(r,1); os.close(r)
print('exact stdout from command',flush=True)
sys.exit(7)
)PY";
pid_t child(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return 0;
  return file.readAll().toInt();
}
bool stopped(pid_t pid) {
  QFile status(QString("/proc/%1/status").arg(pid));
  return status.open(QIODevice::ReadOnly) && status.readAll().contains("T (stopped)");
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  QTemporaryDir root;
  try {
    require(root.isValid(), "Cannot create test root");
    for (const bool new_session : {false, true}) {
      const std::filesystem::path game = root.path().toStdString() + (new_session ? "/pause-session" : "/pause-child");
      const auto token = claim(game);
      const auto workspace = game / "candidate";
      std::filesystem::create_directory(workspace);
      const QString marker = QString::fromStdString(game.string()) + "/worker";
      QProcess process;
      configure(process, game, token);
      process.setProgram("/usr/bin/python3");
      process.setArguments(
          {"-c", "import os,sys,time; open(sys.argv[1],'w').write(str(os.getpid())); time.sleep(30)", marker});
      require(
          hm::ui::PrepareSupervisedProcess(process, game, token, 500, 10, new_session, workspace).ok(),
          "Cannot prepare pause fixture");
      check_workspace(process, workspace);
      process.start();
      require(wait([&] { return child(marker) > 0; }), "Pause worker did not launch");
      const auto worker = child(marker);
      const auto guard = static_cast<pid_t>(process.processId());
      require(::getpgid(worker) == worker && ::getpgid(guard) != worker, "Worker pause group includes the guard");
      require(hm::ui::SignalSupervisedProcess(process, SIGSTOP).ok(), "Cannot request worker Pause");
      require(
          wait([&] {
            const auto paused = hm::ui::SupervisedProcessPaused(process);
            return paused.ok() && *paused;
          }) &&
              stopped(worker) && !stopped(guard),
          "Pause was acknowledged without a stopped worker and responsive guard");
      require(hm::ui::SignalSupervisedProcess(process, SIGCONT).ok(), "Cannot request worker Resume");
      require(
          wait([&] {
            const auto paused = hm::ui::SupervisedProcessPaused(process);
            return paused.ok() && !*paused && !stopped(worker);
          }),
          "Resume did not release worker group");
      require(hm::ui::SignalSupervisedProcess(process, SIGSTOP).ok(), "Cannot repeat worker Pause");
      require(
          wait([&] {
            const auto paused = hm::ui::SupervisedProcessPaused(process);
            return paused.ok() && *paused;
          }),
          "Repeated Pause was not acknowledged");
      require(hm::ui::SignalSupervisedProcess(process, SIGKILL).ok(), "Cannot cancel paused worker");
      require(wait([&] { return process.state() == QProcess::NotRunning; }), "Paused cancellation did not finish");
      require(
          process.exitStatus() == QProcess::NormalExit && process.exitCode() == 128 + SIGKILL &&
              ::kill(worker, 0) == -1 && errno == ESRCH,
          "Paused cancellation did not reap exact worker");
      require(hm::game_operation::ReleaseLocal(game, token, true).ok(), "Paused cancellation lacks cleanup proof");
      check_released(workspace);
    }
    {
      const std::filesystem::path game = root.path().toStdString() + "/normal";
      const auto token = claim(game);
      const auto workspace = game / "candidate";
      std::filesystem::create_directory(workspace);
      const QString marker = root.path() + "/detached-pid";
      QProcess unrelated;
      unrelated.start("/bin/sh", {"-c", "sleep 2; exit 23"});
      require(unrelated.waitForStarted(), "Cannot start unrelated Qt child");
      QProcess process;
      configure(process, game, token);
      process.setProgram("/usr/bin/python3");
      process.setArguments({"-c", kDetached, marker});
      require(
          hm::ui::PrepareSupervisedProcess(process, game, token, 500, 10, true, workspace).ok(),
          "Cannot prepare guarded command");
      check_workspace(process, workspace);
      process.start();
      require(wait([&] { return child(marker) > 0; }), "Detached nondumpable helper did not launch");
      const pid_t pid = child(marker);
      auto proof = hm::game_operation::LocalChildrenStopped(token);
      require(
          proof.ok() && !*proof && !hm::game_operation::ReleaseLocal(game, token, true).ok(),
          "Pending command released admission with a live detached child");
      check_workspace(process, workspace);
      require(wait([&] { return process.state() == QProcess::NotRunning; }), "Guard did not reap its descendants");
      require(
          process.exitStatus() == QProcess::NormalExit && process.exitCode() == 7 &&
              process.readAllStandardOutput().contains("exact stdout from command"),
          "Guard changed command exit status or stdout");
      require(::kill(pid, 0) != 0 && errno == ESRCH, "Guard published completion before descendant reap");
      require(unrelated.state() == QProcess::Running, "Guard stole an unrelated Qt process");
      require(hm::game_operation::ReleaseLocal(game, token, true).ok(), "Verified command could not release admission");
      check_released(workspace);
      require(
          wait([&] { return unrelated.state() == QProcess::NotRunning; }) && unrelated.exitCode() == 23,
          "Unrelated QProcess lost its exact completion status");
    }
    {
      const std::filesystem::path game = root.path().toStdString() + "/unlaunched";
      const auto token = claim(game);
      const auto workspace = game / "candidate";
      std::filesystem::create_directory(workspace);
      QProcess process;
      configure(process, game, token);
      process.setProgram("/bin/true");
      require(
          hm::ui::PrepareSupervisedProcess(process, game, token, 500, 10, true, workspace).ok(),
          "Cannot prepare unlaunched command");
      check_workspace(process, workspace);
      process.setProgram(root.path() + "/missing-supervisor");
      process.start();
      require(
          wait([&] { return process.error() == QProcess::FailedToStart && process.state() == QProcess::NotRunning; }),
          "Missing supervisor did not report FailedToStart");
      const auto stopped = hm::ui::SupervisedProcessStopped(process);
      require(stopped.ok() && *stopped, "FailedToStart completion disappeared before the dialog could observe it");
      check_released(workspace);
      require(
          hm::game_operation::ReleaseLocal(game, token, true).ok(),
          "FailedToStart did not retire only its unlaunched permit");
    }
    {
      const std::filesystem::path game = root.path().toStdString() + "/lost-proof";
      const auto token = claim(game);
      const auto workspace = game / "candidate";
      std::filesystem::create_directory(workspace);
      const QString marker = root.path() + "/lost-proof-pid";
      QProcess process;
      configure(process, game, token);
      process.setProgram("/usr/bin/python3");
      process.setArguments({"-c", kDetached, marker});
      require(
          hm::ui::PrepareSupervisedProcess(process, game, token, 60000, 10, true, workspace).ok(),
          "Cannot prepare lost-proof command");
      process.start();
      require(wait([&] { return child(marker) > 0; }), "Lost-proof helper did not launch");
      const pid_t pid = child(marker);
      process.kill();
      require(wait([&] { return process.state() == QProcess::NotRunning; }), "Test could not kill its guard");
      const auto proof = hm::game_operation::LocalChildrenStopped(token);
      require(
          !proof.ok() && !hm::game_operation::ReleaseLocal(game, token, true).ok(),
          "Killed guard supplied false descendant shutdown authority");
      check_workspace(process, workspace);
      // The deliberately orphaned test child is stopped explicitly. Its dead
      // PID is still not a replacement for the lost supervisor receipt.
      require(::kill(pid, SIGKILL) == 0 || errno == ESRCH, "Cannot stop owned lost-proof test child");
      require(!hm::game_operation::ReleaseLocal(game, token, true).ok(), "Dead helper replaced missing command proof");
      check_workspace(process, workspace);
      hm::game_operation::ForgetAuthorization(game, token);
    }
    {
      const std::filesystem::path game = root.path().toStdString() + "/changed-workspace-owner";
      const auto token = claim(game);
      const auto workspace = game / "candidate";
      std::filesystem::create_directory(workspace);
      QProcess process;
      configure(process, game, token);
      process.setProgram("/bin/true");
      require(
          hm::ui::PrepareSupervisedProcess(process, game, token, 500, 10, true, workspace).ok(),
          "Cannot prepare replacement-owner fixture");
      hm::game_operation::Owner replacement;
      {
        auto lock = hm::game_operation::Lock::Acquire(workspace);
        require(lock.ok(), "Cannot lock private workspace");
        auto owner = (*lock)->Read();
        require(owner.ok() && owner->has_value(), "Workspace owner disappeared");
        replacement = **owner;
        replacement.token = "successor-token";
        require(
            (*lock)->Handoff(replacement, (**owner).token, (**owner).revision).ok(),
            "Cannot stage replacement-owner fixture");
      }
      process.start();
      require(wait([&] { return process.state() == QProcess::NotRunning; }), "Replacement fixture did not finish");
      require(
          !hm::ui::SupervisedProcessStopped(process).ok() && !hm::game_operation::ReleaseLocal(game, token, true).ok(),
          "Command cleanup released a replacement workspace owner");
      auto retained = hm::game_operation::Inspect(workspace);
      require(
          retained.ok() && retained->has_value() && (**retained).token == replacement.token,
          "Command cleanup removed the replacement owner");
      hm::game_operation::ForgetAuthorization(game, token);
    }
    {
      const std::filesystem::path game = root.path().toStdString() + "/busy-workspace";
      const auto token = claim(game);
      const auto workspace = game / "candidate";
      std::filesystem::create_directory(workspace);
      auto existing = hm::game_operation::LocalOwner("pipeline", "other-operation", "other-token");
      {
        auto lock = hm::game_operation::Lock::Acquire(workspace);
        require(lock.ok() && (*lock)->Claim(existing).ok(), "Cannot reserve busy workspace fixture");
      }
      QProcess process;
      configure(process, game, token);
      process.setProgram("/bin/true");
      require(
          !hm::ui::PrepareSupervisedProcess(process, game, token, 500, 10, true, workspace).ok(),
          "Preparation replaced another workspace owner");
      require(
          hm::game_operation::ReleaseLocal(game, token, true).ok(),
          "Failed preparation left an unlaunched command permit");
      const auto retained = hm::game_operation::Inspect(workspace);
      require(
          retained.ok() && retained->has_value() && (**retained).token == existing.token,
          "Failed preparation removed another workspace owner");
    }
    std::cout
        << "Qt command receipts preserve detached-child proof, unrelated QProcess status, FailedToStart and killed-guard fencing\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
