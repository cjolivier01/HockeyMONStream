#include "src/apps/hstream-ui/SourceCameraProcess.h"

#include "src/apps/hstream-ui/SupervisedProcess.h"

#include <fcntl.h>
#include <unistd.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <csignal>
#include <iostream>

namespace hm::ui {
namespace {
QByteArray packet(const QJsonObject& object) {
  return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}
} // namespace

int RunSourceCameraProcess(const QStringList& arguments) {
  // Keep vendor stdout/stderr together and reserve the original pipe for replies.
  QFile replies;
  const int descriptor = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
  if (descriptor < 0 || !replies.open(descriptor, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle) ||
      dup2(STDERR_FILENO, STDOUT_FILENO) < 0 || arguments.size() < 3)
    return 1;
  SourceCameraPlayer player;
  QString error;
  if (!player.Open(arguments.mid(2), arguments[0].toULongLong(), arguments[1].toInt(), &error)) {
    replies.write(packet({{"error", error}}));
    replies.flush();
    return 1;
  }
  std::string line;
  while (std::getline(std::cin, line)) {
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(line));
    if (!document.isObject())
      return 1;
    const auto command = document.object();
    if (command.contains("seek"))
      player.Seek(command["seek"].toInteger());
    if (command.contains("play")) {
      if (command["play"].toBool())
        player.Play();
      else
        player.Pause();
    }
    const auto s = player.Poll();
    const bool redrawn = command["redraw"].toBool() && player.Redraw();
    const auto response = packet(
        {{"playing", s.playing},
         {"ready", s.ready},
         {"seeking", s.seeking},
         {"position", s.position_ms},
         {"duration", s.duration_ms},
         {"pts", s.presented_ns},
         {"width", s.width},
         {"height", s.height},
         {"error", s.error},
         {"redrawn", redrawn}});
    if (replies.write(response) != response.size() || !replies.flush() || !s.error.isEmpty())
      return 1;
  }
  return 0;
}

bool SourceCameraProcess::Open(
    const QStringList& files,
    quint64 window,
    int timeout_ms,
    int kill_timeout_ms,
    const std::filesystem::path& game,
    const std::string& token,
    int64_t grace_ms,
    int64_t poll_ms,
    QString* error) {
  timeout_ms_ = timeout_ms;
  kill_timeout_ms_ = kill_timeout_ms;
  process_ = new QProcess(qApp);
  process_->setObjectName("sourceCameraWorker");
  process_->setProcessChannelMode(QProcess::ForwardedErrorChannel);
  process_->setProcessEnvironment(QProcessEnvironment::systemEnvironment());
  process_->setProgram(QCoreApplication::applicationDirPath() + "/hstream-ui");
  process_->setArguments(
      QStringList{"--source-camera-worker", QString::number(window), QString::number(timeout_ms)} + files);
  const auto prepared = PrepareSupervisedProcess(*process_, game, token, grace_ms, poll_ms, false);
  if (!prepared.ok()) {
    *error = QString::fromStdString(prepared.ToString());
    return false;
  }
  process_->start();
  status_.open = true;
  return true;
}

void SourceCameraProcess::Play() {
  pending_["play"] = status_.playing = true;
}
void SourceCameraProcess::Pause() {
  // Publish Pause with the worker's matching position. The previous poll can
  // still describe moving playback when the user leaves the camera tab.
  pending_["play"] = false;
}
void SourceCameraProcess::Seek(qint64 position) {
  pending_["seek"] = status_.position_ms = position;
  status_.seeking = true;
  status_.presented_ns = -1;
}
bool SourceCameraProcess::Redraw() {
  pending_["redraw"] = true;
  return true;
}

SourceCameraProcess::Status SourceCameraProcess::Poll() {
  if (!process_)
    return status_;
  if (process_->canReadLine()) {
    const auto reply = QJsonDocument::fromJson(process_->readLine()).object();
    awaiting_reply_ = false;
    status_.error = reply["error"].toString();
    if (status_.error.isEmpty() && !reply.contains("duration"))
      status_.error = "The camera worker returned an invalid status.";
    status_.duration_ms = reply["duration"].toInteger();
    status_.width = reply["width"].toInt();
    status_.height = reply["height"].toInt();
    if (!pending_.contains("play"))
      status_.playing = reply["playing"].toBool();
    status_.ready = reply["ready"].toBool();
    if (!pending_.contains("seek")) {
      status_.position_ms = reply["position"].toInteger();
      status_.presented_ns = reply["pts"].toInteger(-1);
      status_.seeking = reply["seeking"].toBool();
    }
    if (retry_redraw_ && !reply["redrawn"].toBool() && !status_.playing)
      pending_["redraw"] = true;
  }
  if (!status_.error.isEmpty())
    return status_;
  if (process_->state() == QProcess::NotRunning) {
    if (process_->error() == QProcess::FailedToStart)
      status_.error = "Could not start camera playback: " + process_->errorString();
    else
      status_.error = QString("Camera playback stopped unexpectedly (worker exit %1).").arg(process_->exitCode());
  } else if (!awaiting_reply_) {
    const auto request = packet(pending_);
    retry_redraw_ = pending_.contains("redraw");
    pending_ = {};
    if (process_->write(request) != request.size())
      status_.error = "Could not send the camera playback command.";
    reply_timer_.restart();
    awaiting_reply_ = true;
  } else if (reply_timer_.elapsed() > timeout_ms_) {
    status_.error = "The camera playback worker stopped responding.";
  }
  return status_;
}

void SourceCameraProcess::Close() {
  if (!process_)
    return;
  if (process_->state() != QProcess::NotRunning) {
    process_->closeWriteChannel();
    if (!process_->waitForFinished(kill_timeout_ms_) && process_->state() != QProcess::NotRunning) {
      (void)SignalSupervisedProcess(*process_, SIGKILL);
      process_->waitForFinished(kill_timeout_ms_);
    }
  }
  // A delayed guard still owns its permit. Leave it alive to reap and publish
  // the receipt before the window's ordinary admission poll can release the game.
  if (process_->state() == QProcess::NotRunning)
    delete process_;
  else
    QObject::connect(process_, &QProcess::finished, process_, &QObject::deleteLater);
  process_ = nullptr;
}

SourceCameraProcess::~SourceCameraProcess() {
  Close();
}
} // namespace hm::ui
