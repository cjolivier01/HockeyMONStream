#include "src/apps/hstream-ui/SupervisedProcess.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>

#include "hstream/src/libs/common/GameOperationOwner.h"

namespace hm::ui {
namespace {
constexpr const char* kAdapterName = "hstreamSupervisedProcess";

class Command final : public QObject {
 public:
  Command(QProcess& process, std::string token, std::string id)
      : QObject(&process), token(std::move(token)), id(std::move(id)) {
    setObjectName(kAdapterName);
    connect(&process, &QProcess::started, this, [this] {
      started = true;
      const auto status = game_operation::SupervisedCommandStarted(this->token, this->id);
      if (!status.ok())
        qWarning("Cannot acknowledge supervised launch: %s", status.ToString().c_str());
    });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
      if (error != QProcess::FailedToStart || started)
        return;
      const auto status = game_operation::CancelUnlaunchedSupervisedCommand(this->token, this->id);
      if (!status.ok())
        qWarning("Cannot retire unlaunched command: %s", status.ToString().c_str());
    });
    // Qt emits this transition before finished. Existing finished handlers may
    // retire a verified owner, so record the exit before they forget its token.
    connect(&process, &QProcess::stateChanged, this, [this](QProcess::ProcessState state) {
      if (state != QProcess::NotRunning || !started)
        return;
      const auto status = game_operation::SupervisedCommandFinished(this->token, this->id);
      if (!status.ok())
        qWarning("Cannot record supervised exit: %s", status.ToString().c_str());
    });
  }
  std::string token, id;
  bool started{false};
};

Command* command(const QProcess& process) {
  return dynamic_cast<Command*>(process.findChild<QObject*>(kAdapterName, Qt::FindDirectChildrenOnly));
}

absl::StatusOr<QString> executable(const QProcess& process) {
  const auto environment = process.processEnvironment();
  const QString explicit_path = environment.value("HSTREAM_PROCESS_GUARD");
  if (!explicit_path.isEmpty()) {
    if (!QFileInfo(explicit_path).isAbsolute() || !QFileInfo(explicit_path).isExecutable())
      return absl::FailedPreconditionError("HSTREAM_PROCESS_GUARD must identify an executable absolute path");
    return explicit_path;
  }
  const QString application = QCoreApplication::applicationFilePath();
  const QString directory = QCoreApplication::applicationDirPath();
  QStringList candidates{
      directory + "/hstream-process-guard",
      directory + "/../hstream-process-guard/hstream-process-guard",
      application + ".runfiles/kstream/src/apps/hstream-process-guard/hstream-process-guard"};
  for (const auto& key : {"RUNFILES_DIR", "TEST_SRCDIR"})
    if (!environment.value(key).isEmpty())
      candidates << environment.value(key) + "/kstream/src/apps/hstream-process-guard/hstream-process-guard";
  candidates
      << QDir(process.workingDirectory()).filePath("bazel-bin/src/apps/hstream-process-guard/hstream-process-guard");
  candidates << QStandardPaths::findExecutable(
      "hstream-process-guard", environment.value("PATH").split(QDir::listSeparator()));
  for (const auto& candidate : candidates)
    if (!candidate.isEmpty() && QFileInfo(candidate).isExecutable())
      return QFileInfo(candidate).absoluteFilePath();
  return absl::FailedPreconditionError("The matching hstream-process-guard executable is unavailable");
}
} // namespace

absl::Status PrepareSupervisedProcess(
    QProcess& process,
    const std::filesystem::path& game,
    const std::string& token,
    int64_t grace_ms,
    int64_t poll_ms,
    bool new_session) {
  if (process.state() != QProcess::NotRunning)
    return absl::FailedPreconditionError("A running process cannot acquire another command permit");
  const auto guard = executable(process);
  if (!guard.ok())
    return guard.status();
  if (new_session && !QFileInfo("/usr/bin/setsid").isExecutable())
    return absl::FailedPreconditionError("The process session launcher is unavailable");
  std::vector<std::string> child{process.program().toStdString()};
  for (const auto& argument : process.arguments())
    child.push_back(argument.toStdString());
  auto permit = game_operation::PrepareSupervisedCommand(game, token, grace_ms, poll_ms, child);
  if (!permit.ok())
    return permit.status();
  delete command(process);
  new Command(process, token, permit->id);
  QStringList arguments;
  if (new_session)
    arguments << *guard;
  for (const auto& argument : permit->arguments)
    arguments << QString::fromStdString(argument);
  process.setProgram(new_session ? "/usr/bin/setsid" : *guard);
  process.setArguments(arguments);
  return absl::OkStatus();
}

absl::Status SignalSupervisedProcess(QProcess& process, int signal) {
  const auto* saved = command(process);
  return saved ? game_operation::SignalSupervisedCommand(saved->token, saved->id, signal)
               : absl::FailedPreconditionError("Process has no supervised command permit");
}

absl::StatusOr<bool> SupervisedProcessStopped(const QProcess& process) {
  const auto* saved = command(process);
  return saved ? game_operation::SupervisedCommandStopped(saved->token, saved->id)
               : absl::StatusOr<bool>(absl::FailedPreconditionError("Process has no supervised command permit"));
}

absl::StatusOr<bool> SupervisedProcessPaused(const QProcess& process) {
  const auto* saved = command(process);
  return saved ? game_operation::SupervisedCommandPaused(saved->token, saved->id)
               : absl::StatusOr<bool>(absl::FailedPreconditionError("Process has no supervised command permit"));
}

bool HasSupervisedProcess(const QProcess& process) {
  return command(process) != nullptr;
}

} // namespace hm::ui
