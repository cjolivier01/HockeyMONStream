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
} // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  QTemporaryDir root;
  try {
    require(root.isValid(), "Cannot create test root");
    {
      const std::filesystem::path game = root.path().toStdString() + "/normal";
      const auto token = claim(game);
      const QString marker = root.path() + "/detached-pid";
      QProcess unrelated;
      unrelated.start("/bin/sh", {"-c", "sleep 2; exit 23"});
      require(unrelated.waitForStarted(), "Cannot start unrelated Qt child");
      QProcess process;
      configure(process, game, token);
      process.setProgram("/usr/bin/python3");
      process.setArguments({"-c", kDetached, marker});
      require(
          hm::ui::PrepareSupervisedProcess(process, game, token, 500, 10, true).ok(), "Cannot prepare guarded command");
      process.start();
      require(wait([&] { return child(marker) > 0; }), "Detached nondumpable helper did not launch");
      const pid_t pid = child(marker);
      auto proof = hm::game_operation::LocalChildrenStopped(token);
      require(
          proof.ok() && !*proof && !hm::game_operation::ReleaseLocal(game, token, true).ok(),
          "Pending command released admission with a live detached child");
      require(wait([&] { return process.state() == QProcess::NotRunning; }), "Guard did not reap its descendants");
      require(
          process.exitStatus() == QProcess::NormalExit && process.exitCode() == 7 &&
              process.readAllStandardOutput().contains("exact stdout from command"),
          "Guard changed command exit status or stdout");
      require(::kill(pid, 0) != 0 && errno == ESRCH, "Guard published completion before descendant reap");
      require(unrelated.state() == QProcess::Running, "Guard stole an unrelated Qt process");
      require(hm::game_operation::ReleaseLocal(game, token, true).ok(), "Verified command could not release admission");
      require(
          wait([&] { return unrelated.state() == QProcess::NotRunning; }) && unrelated.exitCode() == 23,
          "Unrelated QProcess lost its exact completion status");
    }
    {
      const std::filesystem::path game = root.path().toStdString() + "/unlaunched";
      const auto token = claim(game);
      QProcess process;
      configure(process, game, token);
      process.setProgram("/bin/true");
      require(
          hm::ui::PrepareSupervisedProcess(process, game, token, 500, 10, true).ok(),
          "Cannot prepare unlaunched command");
      process.setProgram(root.path() + "/missing-supervisor");
      process.start();
      require(
          wait([&] { return process.error() == QProcess::FailedToStart && process.state() == QProcess::NotRunning; }),
          "Missing supervisor did not report FailedToStart");
      require(
          hm::game_operation::ReleaseLocal(game, token, true).ok(),
          "FailedToStart did not retire only its unlaunched permit");
    }
    {
      const std::filesystem::path game = root.path().toStdString() + "/lost-proof";
      const auto token = claim(game);
      const QString marker = root.path() + "/lost-proof-pid";
      QProcess process;
      configure(process, game, token);
      process.setProgram("/usr/bin/python3");
      process.setArguments({"-c", kDetached, marker});
      require(
          hm::ui::PrepareSupervisedProcess(process, game, token, 60000, 10, true).ok(),
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
      // The deliberately orphaned test child is stopped explicitly. Its dead
      // PID is still not a replacement for the lost supervisor receipt.
      require(::kill(pid, SIGKILL) == 0 || errno == ESRCH, "Cannot stop owned lost-proof test child");
      require(!hm::game_operation::ReleaseLocal(game, token, true).ok(), "Dead helper replaced missing command proof");
      hm::game_operation::ForgetAuthorization(game, token);
    }
    std::cout
        << "Qt command receipts preserve detached-child proof, unrelated QProcess status, FailedToStart and killed-guard fencing\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
